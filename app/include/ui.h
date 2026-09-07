/*
 * Copyright (c) 2026 Josh Gav
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * System-level UI constants: screen geometry, shared palette, and layout
 * metrics that belong to the application as a whole rather than to any one
 * widget. Components take their own dimensions through their config structs,
 * so nothing here should describe a single component's internals.
 */

#ifndef UI_H_
#define UI_H_

#include <zephyr/devicetree.h>

/*
 * Screen size, taken from the panel the devicetree selected rather than
 * hardcoded, so a board overlay that changes the display resolution is
 * picked up automatically.
 *
 * On the RLCD board these are the landscape dimensions - 400x300 - because the
 * display node describes the orientation the application draws into, not the
 * panel's physical portrait raster. The driver rotates between the two.
 */
#define UI_SCREEN_W DT_PROP(DT_CHOSEN(zephyr_display), width)
#define UI_SCREEN_H DT_PROP(DT_CHOSEN(zephyr_display), height)

/*
 * Inset from the screen edge for top-level content.
 *
 * On this panel the value is no longer constrained by the controller. The
 * landscape driver takes full-frame writes only and does the 12-pixel column
 * alignment itself, against the panel's own axis, so a margin in landscape
 * coordinates is just a layout choice - unlike the portrait configuration,
 * where a margin that was not a multiple of 12 made the driver reject the
 * flush outright and the gauge silently never appeared.
 *
 * 12 is kept because the layout was tuned around it and it still looks right.
 */
#define UI_MARGIN 12

/*
 * === Shared palette, 0xRRGGBB ===
 *
 * The reflective LCD is 1 bit per pixel: the board's Kconfig.defconfig pins
 * LVGL to LV_COLOR_DEPTH_1, and the software blender reduces every colour to
 * one bit by luminance - (77*R + 151*G + 28*B) >> 8, lit when the result is
 * above LV_DRAW_SW_I1_LUM_THRESHOLD (127).
 *
 * That makes mid-tones dangerous rather than merely approximate. Two colours
 * that look unrelated can land on the same side of the threshold and become
 * the same pixel: the previous fill (0xff3000) came out at luminance 105 and
 * the background (0x202020) at 32, so both resolved to unlit and the gauge
 * appeared never to fill at all.
 *
 * So the palette is pure black and white, and every entry is chosen for which
 * side of the threshold it lands on rather than for its hue. Anything added
 * here should be checked the same way.
 *
 * A note on the panel: this is a monochrome ST7306. The controller family does
 * support a red-black-white mode, but it is selected by the devicetree
 * color-mode property, which exists only for panels wired with the red
 * segment - this board does not set it, and the driver reports MONO01 as its
 * only pixel format. There is no red to spend, so contrast does the work that
 * a third colour would.
 */

/*
 * Background: white. On a reflective panel this is the "paper" state and is
 * what makes the display readable in bright light, which is the point of the
 * technology. Luminance 255, so it is always above the threshold.
 */
#define UI_COLOR_BG 0xffffff

/* Ink. Luminance 0, always below the threshold. */
#define UI_COLOR_TEXT 0x000000

/*
 * Secondary text and the unfilled track. Also ink - a 1bpp panel has no
 * dimmer shade - so these are distinguished by stroke weight and position
 * rather than by tone.
 */
#define UI_COLOR_DIM 0x000000

/*
 * The filled portion of the gauge.
 *
 * This is where red would go on a red-black-white panel. This board is not
 * one: it has no color-mode property, so the ST730x driver runs it at 1 bit
 * per pixel and reports PIXEL_FORMAT_MONO01 as its only format. Red is a
 * property of the glass, not a setting, so the fill is solid ink instead -
 * the strongest emphasis available here.
 */
#define UI_COLOR_ALERT 0x000000

/*
 * Gauge canvas size: the whole screen.
 *
 * The canvas used to be inset by UI_MARGIN on each side, which left a visible
 * band of background all the way around it - and because the track then insets
 * itself again inside the canvas, the padding was applied twice over. The
 * canvas now covers the panel edge to edge and the only inset is the track's
 * own, so the gauge uses all the glass.
 */
#define UI_GAUGE_W UI_SCREEN_W
#define UI_GAUGE_H UI_SCREEN_H

#endif /* UI_H_ */
