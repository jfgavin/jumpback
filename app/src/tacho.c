#include <errno.h>
#include <zephyr/kernel.h>
#include <lvgl.h>

#include "tacho.h"
#include "ui.h"

/* === Defaults, applied to any config field left zero === */
#define DEF_W             UI_GAUGE_W
#define DEF_H             UI_GAUGE_H
#define DEF_RPM_MAX       8000
#define DEF_TICK_INTERVAL 1000
#define DEF_COLOR_TRACK   UI_COLOR_DIM
#define DEF_COLOR_FILL    UI_COLOR_ALERT

/* === Track === */
#define TRACK_WIDTH 26

/*
 * Thickness of the outline left around the hollow channel. The fill runs
 * inside it, so the visible bar is TRACK_WIDTH - 2 * TRACK_BORDER wide.
 */
#define TRACK_BORDER 3

/*
 * Inset of the track from the canvas edge. Derived from the stroke width so a
 * wider track cannot clip itself; unrelated to UI_MARGIN, which insets
 * top-level content from the screen edge.
 */
#define TRACK_INSET (TRACK_WIDTH / 2 + 2)

/* === Ticks === */
#define TICK_WIDTH 4

/*
 * Ticks are cut through the track in the background colour, so they read as
 * notches in the bar rather than as marks beside it. That means their length
 * is bounded by the track: drawn any longer, the excess would land on the
 * background, where background-coloured ink is invisible. Half the track width
 * exactly spans it.
 */
#define TICK_HALF_LEN (TRACK_WIDTH / 2)

/* === Tick labels === */
#define LABEL_BOX_W 32
#define LABEL_BOX_H 26

/*
 * Distance from the track centreline to the label centre. The label is
 * LABEL_BOX_H tall, so its near edge sits at LABEL_OFFSET - LABEL_BOX_H / 2;
 * that has to clear the track's half width or white text would touch the white
 * track and stop being readable. The + 4 is that clearance.
 */
#define LABEL_OFFSET (TRACK_WIDTH / 2 + LABEL_BOX_H / 2 + 4)

/*
 * How far right the top corner sits from the left edge. Gives the climb a
 * slight lean rather than a hard right angle.
 */
#define CORNER_X 30

/* Points per track. Fixed so the shape needs no allocation. */
#define TRACK_PT_CNT 3

static void track_points(const struct tacho *t, lv_point_precise_t pts[TRACK_PT_CNT])
{
	int32_t w = t->cfg.width;
	int32_t h = t->cfg.height;
	/*
	 * The track is an L sweep laid out for a portrait panel: it starts at
	 * the bottom left, climbs the full height, then runs across the top.
	 *
	 * Both legs are sized from the canvas rather than from a fixed
	 * fraction, so the gauge uses the whole display instead of sitting in a
	 * band. The corner is pulled in by CORNER_X so the climb leans slightly
	 * right, which keeps the rounded join from looking like a square bend.
	 */
	pts[0].x = TRACK_INSET;
	pts[0].y = h - TRACK_INSET;
	pts[1].x = TRACK_INSET + CORNER_X;
	pts[1].y = TRACK_INSET;
	pts[2].x = w - TRACK_INSET;
	pts[2].y = TRACK_INSET;
}

static int32_t seg_len(const lv_point_precise_t *a, const lv_point_precise_t *b)
{
	int32_t dx = (int32_t)(b->x - a->x);
	int32_t dy = (int32_t)(b->y - a->y);

	/*
	 * Axis-aligned segments avoid the square root. Written generally so the
	 * diagonal segment in the middle of the track still measures correctly.
	 */
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

	for (uint32_t i = 0; i + 1 < TRACK_PT_CNT; i++) {
		total += seg_len(&pts[i], &pts[i + 1]);
	}

	return total;
}

/* Map an RPM onto a distance along the track. */
static int32_t rpm_to_dist(const struct tacho *t, int32_t rpm, int32_t total)
{
	return (total * (rpm - t->cfg.rpm_min)) / (t->cfg.rpm_max - t->cfg.rpm_min);
}

/*
 * Resolve a distance along the polyline into a point, and report the unit
 * normal of the segment it lands on (scaled by 1024, since there is no float
 * here). The normal is what lets ticks be drawn across the track and labels be
 * pushed clear of it, at any segment angle.
 */
static bool track_point_at(const lv_point_precise_t *pts, int32_t dist, lv_point_t *out,
			   int32_t *nx_q10, int32_t *ny_q10)
{
	int32_t remaining = dist;

	for (uint32_t i = 0; i + 1 < TRACK_PT_CNT; i++) {
		const lv_point_precise_t *a = &pts[i];
		const lv_point_precise_t *b = &pts[i + 1];
		int32_t len = seg_len(a, b);

		if (len == 0) {
			continue;
		}

		/*
		 * The <= lets a distance landing exactly on a corner resolve on
		 * the earlier segment, so the final tick at full scale is placed
		 * rather than falling off the end of the loop.
		 */
		if (remaining <= len) {
			int32_t dx = (int32_t)(b->x - a->x);
			int32_t dy = (int32_t)(b->y - a->y);

			out->x = (int32_t)a->x + (dx * remaining) / len;
			out->y = (int32_t)a->y + (dy * remaining) / len;

			/* Normal of (dx, dy) is (-dy, dx), normalised in Q10. */
			*nx_q10 = (-dy * 1024) / len;
			*ny_q10 = (dx * 1024) / len;

			return true;
		}

		remaining -= len;
	}

	return false;
}

/*
 * Draw the polyline from its start until `fill_len` pixels have been covered.
 * Whole segments are drawn as-is; the segment the fill ends inside is drawn
 * short, to a point interpolated along it.
 */
static void draw_partial(lv_layer_t *layer, lv_draw_line_dsc_t *dsc,
			 const lv_point_precise_t *pts, int32_t fill_len)
{
	int32_t remaining = fill_len;

	for (uint32_t i = 0; i + 1 < TRACK_PT_CNT && remaining > 0; i++) {
		const lv_point_precise_t *a = &pts[i];
		const lv_point_precise_t *b = &pts[i + 1];
		int32_t len = seg_len(a, b);

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

/* Ticks across the track at each interval, labelled with the thousands digit. */
static void draw_ticks(const struct tacho *t, lv_layer_t *layer,
		       const lv_point_precise_t *pts, int32_t total)
{
	lv_draw_line_dsc_t tick;
	lv_draw_label_dsc_t label;
	char text[8];

	lv_draw_line_dsc_init(&tick);
	tick.width = TICK_WIDTH;
	tick.opa = LV_OPA_COVER;
	tick.color = lv_color_hex(UI_COLOR_BG);

	lv_draw_label_dsc_init(&label);
	label.color = lv_color_hex(UI_COLOR_TEXT);
	/*
	 * A larger face than LV_FONT_DEFAULT. There is no antialiasing to lean
	 * on at 1bpp - every glyph pixel is fully on or off - so small text
	 * breaks up badly. Bigger strokes survive the reduction to one bit.
	 */
	label.font = &lv_font_montserrat_20;
	label.align = LV_TEXT_ALIGN_CENTER;
	label.text = text;

	/*
	 * lv_draw_label() only queues a task; nothing renders until
	 * lv_canvas_finish_layer(). The task keeps whatever `text` points at, so
	 * without this every label would render the buffer's final contents -
	 * and `text` is a local, so the pointer would dangle once this function
	 * returns. text_local makes LVGL strdup the string per task instead.
	 */
	label.text_local = 1;

	for (int32_t rpm = t->cfg.rpm_min; rpm <= t->cfg.rpm_max; rpm += t->cfg.tick_interval) {
		lv_point_t pt;
		int32_t nx, ny;
		int32_t lx, ly;
		lv_area_t box;

		if (!track_point_at(pts, rpm_to_dist(t, rpm, total), &pt, &nx, &ny)) {
			continue;
		}

		tick.p1.x = pt.x - (nx * TICK_HALF_LEN) / 1024;
		tick.p1.y = pt.y - (ny * TICK_HALF_LEN) / 1024;
		tick.p2.x = pt.x + (nx * TICK_HALF_LEN) / 1024;
		tick.p2.y = pt.y + (ny * TICK_HALF_LEN) / 1024;
		lv_draw_line(layer, &tick);

		/* The leading number: 7000 RPM reads "7", 11000 reads "11". */
		lv_snprintf(text, sizeof(text), "%d", (int)(rpm / 1000));

		/* Offset along the normal so the label sits clear of the track. */
		lx = pt.x + (nx * LABEL_OFFSET) / 1024;
		ly = pt.y + (ny * LABEL_OFFSET) / 1024;

		box.x1 = lx - LABEL_BOX_W / 2;
		box.x2 = lx + LABEL_BOX_W / 2;
		box.y1 = ly - LABEL_BOX_H / 2;
		box.y2 = ly + LABEL_BOX_H / 2;

		lv_draw_label(layer, &label, &box);
	}
}

static void tacho_redraw(const struct tacho *t)
{
	lv_point_precise_t pts[TRACK_PT_CNT];
	lv_layer_t layer;
	lv_draw_line_dsc_t dsc;
	int32_t total;

	track_points(t, pts);
	total = track_total_len(pts);

	lv_canvas_fill_bg(t->canvas, lv_color_hex(UI_COLOR_BG), LV_OPA_COVER);
	lv_canvas_init_layer(t->canvas, &layer);

	lv_draw_line_dsc_init(&dsc);
	dsc.opa = LV_OPA_COVER;
	/* Rounded joins stop the corner showing a notch where segments meet. */
	dsc.round_start = 1;
	dsc.round_end = 1;

	/*
	 * On a 1bpp panel the fill cannot simply be drawn over the track in a
	 * different colour - there are only two colours, so "track" and "fill"
	 * would either be identical or the track would be invisible against the
	 * background. Instead the track is drawn as an outline and the fill as
	 * solid ink inside it:
	 *
	 *   1. the full track at full width in the track colour, forming the
	 *      outline,
	 *   2. the same path inset by the border width in the background
	 *      colour, hollowing it out,
	 *   3. the filled portion, inset the same way, in the fill colour.
	 *
	 * An empty gauge then reads as an outlined channel and a full one as a
	 * solid bar, which survives the reduction to one bit.
	 */

	/* 1. Outline: the whole track, full width. */
	dsc.width = TRACK_WIDTH;
	dsc.color = lv_color_hex(t->cfg.color_track);
	draw_partial(&layer, &dsc, pts, total);

	/* 2. Hollow it out, leaving TRACK_BORDER of outline on each side. */
	dsc.width = TRACK_WIDTH - 2 * TRACK_BORDER;
	dsc.color = lv_color_hex(UI_COLOR_BG);
	draw_partial(&layer, &dsc, pts, total);

	/* 3. Filled portion, inside the hollow. */
	dsc.color = lv_color_hex(t->cfg.color_fill);
	draw_partial(&layer, &dsc, pts, rpm_to_dist(t, t->rpm, total));

	/* Ticks and labels last, so they stay legible over the fill. */
	draw_ticks(t, &layer, pts, total);

	lv_canvas_finish_layer(t->canvas, &layer);
}

/* Fill in any config field the caller left zero. */
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

int tacho_init(struct tacho *t, lv_obj_t *parent, const struct tacho_config *cfg,
	       void *buf, size_t buf_size)
{
	static const struct tacho_config empty;
	size_t needed;

	if (t == NULL || parent == NULL || buf == NULL) {
		return -EINVAL;
	}

	t->cfg = (cfg != NULL) ? *cfg : empty;
	apply_defaults(&t->cfg);

	if (t->cfg.rpm_max <= t->cfg.rpm_min || t->cfg.tick_interval <= 0) {
		return -EINVAL;
	}

	needed = TACHO_BUF_SIZE(t->cfg.width, t->cfg.height);
	if (buf_size < needed) {
		return -ENOMEM;
	}

	t->buf = buf;
	t->rpm = t->cfg.rpm_min;

	t->canvas = lv_canvas_create(parent);
	if (t->canvas == NULL) {
		return -ENOMEM;
	}

	lv_canvas_set_buffer(t->canvas, t->buf, t->cfg.width, t->cfg.height,
			     TACHO_COLOR_FORMAT);

#if LV_COLOR_DEPTH == 1
	/*
	 * An I1 canvas stores one bit per pixel and resolves it through a
	 * two-entry palette, which starts undefined - without this the gauge
	 * renders with whatever the palette happened to contain. Index 0 is the
	 * background, index 1 the ink, matching the two values the blender
	 * picks by luminance.
	 */
	lv_canvas_set_palette(t->canvas, 0, lv_color_to_32(lv_color_hex(UI_COLOR_BG), LV_OPA_COVER));
	lv_canvas_set_palette(t->canvas, 1, lv_color_to_32(lv_color_hex(UI_COLOR_TEXT), LV_OPA_COVER));
#endif

	lv_obj_center(t->canvas);

	tacho_redraw(t);

	return 0;
}

void tacho_set_rpm(struct tacho *t, int32_t rpm)
{
	if (rpm < t->cfg.rpm_min) {
		rpm = t->cfg.rpm_min;
	} else if (rpm > t->cfg.rpm_max) {
		rpm = t->cfg.rpm_max;
	}

	/* Redrawing is the expensive part, so skip it when nothing moved. */
	if (rpm == t->rpm) {
		return;
	}

	t->rpm = rpm;
	tacho_redraw(t);
}

int32_t tacho_get_rpm(const struct tacho *t)
{
	return t->rpm;
}

int32_t tacho_get_rpm_max(const struct tacho *t)
{
	return t->cfg.rpm_max;
}
