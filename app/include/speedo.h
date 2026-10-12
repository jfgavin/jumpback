/*
 * Copyright (c) 2026 Josh Gav
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPEEDO_H_
#define SPEEDO_H_

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>

struct speedo {
	lv_obj_t *value;
	lv_obj_t *caption;
	int32_t shown_mph;
	bool shown_invalid;
};

int speedo_init(struct speedo *s, lv_obj_t *parent);

void speedo_update(struct speedo *s, uint32_t mph_tenths, bool valid);

#endif /* SPEEDO_H_ */
