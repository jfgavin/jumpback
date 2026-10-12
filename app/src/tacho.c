/*
 * Copyright (c) 2026 Josh Gav
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <lvgl.h>

#include "tacho.h"
#include "ui.h"

#define DEF_W             UI_GAUGE_W
#define DEF_H             UI_GAUGE_H
#define DEF_RPM_MAX       8000
#define DEF_TICK_INTERVAL 1000
#define DEF_COLOR_TRACK   UI_COLOR_DIM
#define DEF_COLOR_FILL    UI_COLOR_ALERT

#define TRACK_WIDTH        26
#define TRACK_BORDER       3
#define TRACK_HOLLOW_WIDTH (TRACK_WIDTH - 2 * TRACK_BORDER)
#define TRACK_EDGE_MARGIN  2
#define TRACK_INSET        (TRACK_WIDTH / 2 + TRACK_EDGE_MARGIN)
#define TRACK_CORNER_LEAN  30
#define TRACK_POINT_COUNT  3

#define TICK_WIDTH    4
#define TICK_HALF_LEN (TRACK_WIDTH / 2)

#define LABEL_FONT            lv_font_montserrat_20
#define LABEL_BOX_W           32
#define LABEL_BOX_H           26
#define LABEL_TRACK_CLEARANCE 4
#define LABEL_OFFSET          (TRACK_WIDTH / 2 + LABEL_BOX_H / 2 + LABEL_TRACK_CLEARANCE)
#define LABEL_RPM_PER_DIGIT   1000
#define LABEL_TEXT_LEN        8

#define DIRTY_SLACK 1
#define DIRTY_PAD   (TRACK_WIDTH / 2 + TICK_HALF_LEN + DIRTY_SLACK)

#define NORMAL_UNIT 1024

#define I1_INDEX_0_FILL 0x00
#define I1_INDEX_1_FILL 0xff

struct track_point {
	lv_point_t point;
	int32_t normal_x;
	int32_t normal_y;
};

static lv_area_t tacho_dirty;
static bool tacho_dirty_valid;

static lv_area_t tacho_extra;
static bool tacho_extra_valid;

static void track_points(const struct tacho *t, lv_point_precise_t pts[TRACK_POINT_COUNT])
{
	const int32_t w = t->cfg.width;
	const int32_t h = t->cfg.height;

	pts[0].x = TRACK_INSET;
	pts[0].y = h - TRACK_INSET;
	pts[1].x = TRACK_INSET + TRACK_CORNER_LEAN;
	pts[1].y = TRACK_INSET;
	pts[2].x = w - TRACK_INSET;
	pts[2].y = TRACK_INSET;
}

static int32_t seg_len(const lv_point_precise_t *a, const lv_point_precise_t *b)
{
	const int32_t dx = (int32_t)(b->x - a->x);
	const int32_t dy = (int32_t)(b->y - a->y);

	if (dx == 0) {
		return dy < 0 ? -dy : dy;
	}
	if (dy == 0) {
		return dx < 0 ? -dx : dx;
	}

	return (int32_t)lv_sqrt32((uint32_t)(dx * dx + dy * dy));
}

static int32_t track_total_len(const lv_point_precise_t *pts)
{
	int32_t total = 0;

	for (uint32_t i = 0; i + 1 < TRACK_POINT_COUNT; i++) {
		total += seg_len(&pts[i], &pts[i + 1]);
	}

	return total;
}

static int32_t rpm_to_dist(const struct tacho *t, int32_t rpm, int32_t total)
{
	return (total * (rpm - t->cfg.rpm_min)) / (t->cfg.rpm_max - t->cfg.rpm_min);
}

static bool track_point_at(const lv_point_precise_t *pts, int32_t dist, struct track_point *out)
{
	int32_t remaining = dist;

	for (uint32_t i = 0; i + 1 < TRACK_POINT_COUNT; i++) {
		const lv_point_precise_t *a = &pts[i];
		const lv_point_precise_t *b = &pts[i + 1];
		const int32_t len = seg_len(a, b);

		if (len == 0) {
			continue;
		}

		if (remaining <= len) {
			const int32_t dx = (int32_t)(b->x - a->x);
			const int32_t dy = (int32_t)(b->y - a->y);

			out->point.x = (int32_t)a->x + (dx * remaining) / len;
			out->point.y = (int32_t)a->y + (dy * remaining) / len;
			out->normal_x = (-dy * NORMAL_UNIT) / len;
			out->normal_y = (dx * NORMAL_UNIT) / len;

			return true;
		}

		remaining -= len;
	}

	return false;
}

static lv_point_precise_t along_normal(const struct track_point *tp, int32_t distance)
{
	return (lv_point_precise_t){
		.x = tp->point.x + (tp->normal_x * distance) / NORMAL_UNIT,
		.y = tp->point.y + (tp->normal_y * distance) / NORMAL_UNIT,
	};
}

static void draw_partial(lv_layer_t *layer, lv_draw_line_dsc_t *dsc,
			 const lv_point_precise_t *pts, int32_t fill_len)
{
	int32_t remaining = fill_len;

	for (uint32_t i = 0; i + 1 < TRACK_POINT_COUNT && remaining > 0; i++) {
		const lv_point_precise_t *a = &pts[i];
		const lv_point_precise_t *b = &pts[i + 1];
		const int32_t len = seg_len(a, b);

		if (len == 0) {
			continue;
		}

		dsc->p1 = *a;

		if (remaining >= len) {
			dsc->p2 = *b;
		} else {
			dsc->p2.x = a->x + ((b->x - a->x) * remaining) / len;
			dsc->p2.y = a->y + ((b->y - a->y) * remaining) / len;
		}

		lv_draw_line(layer, dsc);
		remaining -= len;
	}
}

static void draw_ticks(const struct tacho *t, lv_layer_t *layer, const lv_point_precise_t *pts,
		       int32_t total)
{
	lv_draw_line_dsc_t tick;
	lv_draw_label_dsc_t label;
	char text[LABEL_TEXT_LEN];

	lv_draw_line_dsc_init(&tick);
	tick.width = TICK_WIDTH;
	tick.opa = LV_OPA_COVER;
	tick.color = lv_color_hex(UI_COLOR_BG);

	lv_draw_label_dsc_init(&label);
	label.color = lv_color_hex(UI_COLOR_TEXT);
	label.font = &LABEL_FONT;
	label.align = LV_TEXT_ALIGN_CENTER;
	label.text = text;
	label.text_local = 1;

	for (int32_t rpm = t->cfg.rpm_min; rpm <= t->cfg.rpm_max; rpm += t->cfg.tick_interval) {
		struct track_point tp;

		if (!track_point_at(pts, rpm_to_dist(t, rpm, total), &tp)) {
			continue;
		}

		tick.p1 = along_normal(&tp, -TICK_HALF_LEN);
		tick.p2 = along_normal(&tp, TICK_HALF_LEN);
		lv_draw_line(layer, &tick);

		const lv_point_precise_t centre = along_normal(&tp, LABEL_OFFSET);
		const lv_area_t box = {
			.x1 = centre.x - LABEL_BOX_W / 2,
			.x2 = centre.x + LABEL_BOX_W / 2,
			.y1 = centre.y - LABEL_BOX_H / 2,
			.y2 = centre.y + LABEL_BOX_H / 2,
		};

		lv_snprintf(text, sizeof(text), "%d", (int)(rpm / LABEL_RPM_PER_DIGIT));
		lv_draw_label(layer, &label, &box);
	}
}

#if LV_COLOR_DEPTH == 1
static uint8_t i1_fill_byte(lv_color_t color)
{
	const bool is_index_1 = lv_color_luminance(color) > LV_DRAW_SW_I1_LUM_THRESHOLD;

	return is_index_1 ? I1_INDEX_1_FILL : I1_INDEX_0_FILL;
}

static void canvas_clear(lv_obj_t *canvas, lv_color_t color)
{
	lv_draw_buf_t *draw_buf = lv_canvas_get_draw_buf(canvas);
	uint8_t *pixels;

	if (draw_buf == NULL) {
		return;
	}

	pixels = lv_draw_buf_goto_xy(draw_buf, 0, 0);
	if (pixels == NULL) {
		return;
	}

	lv_memset(pixels, i1_fill_byte(color), draw_buf->header.stride * draw_buf->header.h);
	lv_draw_buf_flush_cache(draw_buf, NULL);
}
#else
static void canvas_clear(lv_obj_t *canvas, lv_color_t color)
{
	lv_canvas_fill_bg(canvas, color, LV_OPA_COVER);
}
#endif

static void area_union(lv_area_t *into, const lv_area_t *other)
{
	into->x1 = MIN(into->x1, other->x1);
	into->y1 = MIN(into->y1, other->y1);
	into->x2 = MAX(into->x2, other->x2);
	into->y2 = MAX(into->y2, other->y2);
}

void tacho_add_dirty(const lv_area_t *area)
{
	if (!tacho_extra_valid) {
		tacho_extra = *area;
		tacho_extra_valid = true;
		return;
	}

	area_union(&tacho_extra, area);
}

bool tacho_dirty_span(lv_area_t *out)
{
	const bool have_extra = tacho_extra_valid;
	const lv_area_t extra = tacho_extra;

	tacho_extra_valid = false;

	if (!tacho_dirty_valid) {
		return false;
	}

	*out = tacho_dirty;

	if (have_extra) {
		area_union(out, &extra);
	}

	return true;
}

static void mark_everything_dirty(const struct tacho *t)
{
	tacho_dirty_valid = false;
	lv_obj_invalidate(t->canvas);
}

static void mark_fill_change_dirty(const struct tacho *t, const lv_point_precise_t *pts,
				   int32_t total, int32_t old_rpm, int32_t new_rpm)
{
	struct track_point old_end;
	struct track_point new_end;

	if (!track_point_at(pts, rpm_to_dist(t, old_rpm, total), &old_end) ||
	    !track_point_at(pts, rpm_to_dist(t, new_rpm, total), &new_end)) {
		mark_everything_dirty(t);
		return;
	}

	tacho_dirty = (lv_area_t){
		.x1 = MAX(MIN(old_end.point.x, new_end.point.x) - DIRTY_PAD, 0),
		.y1 = MAX(MIN(old_end.point.y, new_end.point.y) - DIRTY_PAD, 0),
		.x2 = MIN(MAX(old_end.point.x, new_end.point.x) + DIRTY_PAD, t->cfg.width - 1),
		.y2 = MIN(MAX(old_end.point.y, new_end.point.y) + DIRTY_PAD, t->cfg.height - 1),
	};
	tacho_dirty_valid = true;
}

static void tacho_render(const struct tacho *t, const lv_point_precise_t *pts, int32_t total)
{
	lv_layer_t layer;
	lv_draw_line_dsc_t dsc;

	canvas_clear(t->canvas, lv_color_hex(UI_COLOR_BG));
	lv_canvas_init_layer(t->canvas, &layer);

	lv_draw_line_dsc_init(&dsc);
	dsc.opa = LV_OPA_COVER;
	dsc.round_start = 1;
	dsc.round_end = 1;

	dsc.width = TRACK_WIDTH;
	dsc.color = lv_color_hex(t->cfg.color_track);
	draw_partial(&layer, &dsc, pts, total);

	dsc.width = TRACK_HOLLOW_WIDTH;
	dsc.color = lv_color_hex(UI_COLOR_BG);
	draw_partial(&layer, &dsc, pts, total);

	dsc.color = lv_color_hex(t->cfg.color_fill);
	draw_partial(&layer, &dsc, pts, rpm_to_dist(t, t->rpm, total));

	draw_ticks(t, &layer, pts, total);

	lv_canvas_finish_layer(t->canvas, &layer);
}

static void apply_defaults(struct tacho_config *cfg)
{
	if (cfg->width == 0) {
		cfg->width = DEF_W;
	}
	if (cfg->height == 0) {
		cfg->height = DEF_H;
	}
	if (cfg->rpm_max == 0) {
		cfg->rpm_max = DEF_RPM_MAX;
	}
	if (cfg->tick_interval == 0) {
		cfg->tick_interval = DEF_TICK_INTERVAL;
	}
	if (cfg->color_track == 0) {
		cfg->color_track = DEF_COLOR_TRACK;
	}
	if (cfg->color_fill == 0) {
		cfg->color_fill = DEF_COLOR_FILL;
	}
}

int tacho_init(struct tacho *t, lv_obj_t *parent, const struct tacho_config *cfg, void *buf,
	       size_t buf_size)
{
	static const struct tacho_config all_defaults;
	lv_point_precise_t pts[TRACK_POINT_COUNT];

	if (t == NULL || parent == NULL || buf == NULL) {
		return -EINVAL;
	}

	t->cfg = (cfg != NULL) ? *cfg : all_defaults;
	apply_defaults(&t->cfg);

	if (t->cfg.rpm_max <= t->cfg.rpm_min || t->cfg.tick_interval <= 0) {
		return -EINVAL;
	}

	if (buf_size < TACHO_BUF_SIZE(t->cfg.width, t->cfg.height)) {
		return -ENOMEM;
	}

	t->buf = buf;
	t->rpm = t->cfg.rpm_min;

	t->canvas = lv_canvas_create(parent);
	if (t->canvas == NULL) {
		return -ENOMEM;
	}

	lv_canvas_set_buffer(t->canvas, t->buf, t->cfg.width, t->cfg.height, TACHO_COLOR_FORMAT);

#if LV_COLOR_DEPTH == 1
	lv_canvas_set_palette(t->canvas, 0,
			      lv_color_to_32(lv_color_hex(UI_COLOR_BG), LV_OPA_COVER));
	lv_canvas_set_palette(t->canvas, 1,
			      lv_color_to_32(lv_color_hex(UI_COLOR_TEXT), LV_OPA_COVER));
#endif

	lv_obj_center(t->canvas);

	track_points(t, pts);
	tacho_render(t, pts, track_total_len(pts));
	mark_everything_dirty(t);

	return 0;
}

void tacho_set_rpm(struct tacho *t, int32_t rpm)
{
	lv_point_precise_t pts[TRACK_POINT_COUNT];
	int32_t total;

	rpm = CLAMP(rpm, t->cfg.rpm_min, t->cfg.rpm_max);

	if (rpm == t->rpm) {
		return;
	}

	const int32_t old_rpm = t->rpm;

	t->rpm = rpm;

	track_points(t, pts);
	total = track_total_len(pts);
	tacho_render(t, pts, total);
	mark_fill_change_dirty(t, pts, total, old_rpm, rpm);
}

int32_t tacho_get_rpm(const struct tacho *t)
{
	return t->rpm;
}

int32_t tacho_get_rpm_max(const struct tacho *t)
{
	return t->cfg.rpm_max;
}
