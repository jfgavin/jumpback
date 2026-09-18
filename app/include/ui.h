/*
 * Copyright (c) 2026 Josh Gav
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef UI_H_
#define UI_H_

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/display/st7306.h>

#define UI_SCREEN_W DT_PROP(DT_CHOSEN(zephyr_display), width)
#define UI_SCREEN_H DT_PROP(DT_CHOSEN(zephyr_display), height)

#define UI_COLOR_WHITE 0xffffff
#define UI_COLOR_BLACK 0x000000

#define UI_COLOR_BG    UI_COLOR_WHITE
#define UI_COLOR_TEXT  UI_COLOR_BLACK
#define UI_COLOR_DIM   UI_COLOR_BLACK
#define UI_COLOR_ALERT UI_COLOR_BLACK

#define UI_GAUGE_W UI_SCREEN_W
#define UI_GAUGE_H UI_SCREEN_H

#define UI_FLUSH_ALIGN_X BITS_PER_BYTE
#define UI_FLUSH_ALIGN_Y ST7306_PIXELS_PER_COL_ADDR

#endif /* UI_H_ */
