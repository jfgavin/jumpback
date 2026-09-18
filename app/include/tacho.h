/*
 * Copyright (c) 2026 Josh Gav
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TACHO_H_
#define TACHO_H_

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>

struct tacho_config {
	int32_t width;
	int32_t height;
	int32_t rpm_min;
	int32_t rpm_max;
	int32_t tick_interval;
	uint32_t color_track;
	uint32_t color_fill;
};

struct tacho {
	lv_obj_t *canvas;
	struct tacho_config cfg;
	int32_t rpm;
	void *buf;
};

#if LV_COLOR_DEPTH == 1
#define TACHO_COLOR_FORMAT LV_COLOR_FORMAT_I1
#define TACHO_BPP          1
#else
#define TACHO_COLOR_FORMAT LV_COLOR_FORMAT_RGB565
#define TACHO_BPP          16
#endif

#define TACHO_BUF_SIZE(w, h) LV_CANVAS_BUF_SIZE(w, h, TACHO_BPP, LV_DRAW_BUF_STRIDE_ALIGN)

int tacho_init(struct tacho *t, lv_obj_t *parent, const struct tacho_config *cfg, void *buf,
	       size_t buf_size);

void tacho_set_rpm(struct tacho *t, int32_t rpm);

int32_t tacho_get_rpm(const struct tacho *t);

int32_t tacho_get_rpm_max(const struct tacho *t);

bool tacho_dirty_span(lv_area_t *out);

#endif /* TACHO_H_ */
