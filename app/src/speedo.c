/*
 * Copyright (c) 2026 Josh Gav
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <lvgl.h>

#include "speedo.h"
#include "tacho.h"
#include "ui.h"

#define SPEEDO_W      200
#define SPEEDO_H      60
#define CAPTION_H     24
#define SPEEDO_MARGIN 16

#define VALUE_FONT   lv_font_montserrat_48
#define CAPTION_FONT lv_font_montserrat_20
#define CAPTION_TEXT "MPH"
#define NO_FIX_TEXT  "---"

#define MPH_TENTHS_PER_MPH 10U
#define SPEEDO_MPH_MAX     999
#define SPEEDO_MPH_DIGITS  3

#define SHOWN_MPH_NONE (-1)

static void speedo_mark_dirty(const struct speedo *s)
{
	lv_area_t area;
	lv_area_t caption_area;

	lv_obj_get_coords(s->value, &area);
	lv_obj_get_coords(s->caption, &caption_area);

	area.x1 = MIN(area.x1, caption_area.x1);
	area.y1 = MIN(area.y1, caption_area.y1);
	area.x2 = MAX(area.x2, caption_area.x2);
	area.y2 = MAX(area.y2, caption_area.y2);

	tacho_add_dirty(&area);
}

static lv_obj_t *speedo_label_create(lv_obj_t *parent, int32_t height, int32_t bottom_offset,
				     const lv_font_t *font, uint32_t color)
{
	lv_obj_t *label = lv_label_create(parent);

	if (label == NULL) {
		return NULL;
	}

	lv_obj_set_size(label, SPEEDO_W, height);
	lv_obj_align(label, LV_ALIGN_BOTTOM_RIGHT, -SPEEDO_MARGIN, -bottom_offset);
	lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
	lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
	lv_obj_set_style_bg_color(label, lv_color_hex(UI_COLOR_BG), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);

	return label;
}

int speedo_init(struct speedo *s, lv_obj_t *parent)
{
	if (s == NULL || parent == NULL) {
		return -EINVAL;
	}

	*s = (struct speedo){
		.shown_mph = SHOWN_MPH_NONE,
	};

	s->value = speedo_label_create(parent, SPEEDO_H, SPEEDO_MARGIN + CAPTION_H, &VALUE_FONT,
				       UI_COLOR_TEXT);
	if (s->value == NULL) {
		return -ENOMEM;
	}

	s->caption = speedo_label_create(parent, CAPTION_H, SPEEDO_MARGIN, &CAPTION_FONT,
					 UI_COLOR_DIM);
	if (s->caption == NULL) {
		return -ENOMEM;
	}

	lv_label_set_text(s->caption, CAPTION_TEXT);
	lv_label_set_text(s->value, NO_FIX_TEXT);
	s->shown_invalid = true;

	speedo_mark_dirty(s);

	return 0;
}

void speedo_update(struct speedo *s, uint32_t mph_tenths, bool valid)
{
	char text[SPEEDO_MPH_DIGITS + 1];

	if (!valid) {
		if (s->shown_invalid) {
			return;
		}

		lv_label_set_text(s->value, NO_FIX_TEXT);
		s->shown_invalid = true;
		s->shown_mph = SHOWN_MPH_NONE;
		speedo_mark_dirty(s);
		return;
	}

	const int32_t mph =
		MIN((int32_t)DIV_ROUND_CLOSEST(mph_tenths, MPH_TENTHS_PER_MPH), SPEEDO_MPH_MAX);

	if (!s->shown_invalid && mph == s->shown_mph) {
		return;
	}

	snprintf(text, sizeof(text), "%d", mph);
	lv_label_set_text(s->value, text);

	s->shown_mph = mph;
	s->shown_invalid = false;

	speedo_mark_dirty(s);
}
