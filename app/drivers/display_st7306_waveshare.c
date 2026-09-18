/*
 * Copyright (c) 2026 Josh Gav
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Landscape display driver for the Waveshare ESP32-S3-RLCD-4.2 (ST7306).
 *
 * === Why this driver exists ===
 *
 * The panel is physically portrait. The ST7306 drives 720 source lines and
 * 480 gate lines, and this board wires 300 sources and 400 gates of them to
 * glass, so the hardware raster is 300 wide by 400 tall. Nothing in the
 * controller can change that: MADCTL's MV bit is documented as "Page/Column
 * Order", and it only selects whether the RAM address counter auto-increments
 * along X or along Y during a write (datasheet 7.4, "vertical addressing mode
 * (MV=1) ... Y-address increments after each byte"). It re-orders the write
 * scan, it does not re-wire sources to gates. There is no hardware landscape
 * mode to switch on, so 400x300 has to be produced by rotating pixels.
 *
 * The rotation cannot live in LVGL either. This board pins LVGL to
 * LV_COLOR_DEPTH_1 (see the board's Kconfig.defconfig), and LVGL 9.6's
 * lv_draw_sw_rotate() implements L8, RGB565, RGB888 and ARGB8888 only - I1
 * falls off the end of the switch and the function returns having written
 * nothing. lv_display_set_rotation() on a 1bpp display therefore yields a
 * blank panel rather than an error, which is worse than not supporting it.
 *
 * So the rotation belongs here, in the one place that already has to walk
 * every pixel: the format conversion.
 *
 * === Why rotating is a win, not just a workaround ===
 *
 * The upstream st730x driver derives its row stride from the display width,
 * which forces the width to be a multiple of both 12 (the controller
 * addresses columns in groups of 12) and 8 (a packed 1bpp row must be a whole
 * number of bytes). 300 is a multiple of 12 but not of 8 - 300/8 = 37.5 bytes
 * - so the portrait configuration has to shrink the width to 288 and abandon
 * the 12 rightmost columns of glass.
 *
 * Rotating swaps which axis carries which constraint, and both then hold
 * exactly:
 *
 *   - The LVGL buffer is 400 wide. 400/8 = 50 bytes per row, exact, so the
 *     packed rows need no padding and no truncation.
 *   - Those 400 logical columns map to the panel's 400 rows, addressed two
 *     at a time: 400/2 = 200 row addresses, exact.
 *   - The 300 logical rows map to the panel's 300 columns, addressed twelve
 *     at a time: 300/12 = 25 column addresses, exact.
 *
 * Landscape uses the entire 300x400 glass with no lost columns.
 *
 * === Relationship to the upstream driver ===
 *
 * This is a separate driver rather than a patch to drivers/display/st730x.c
 * because the rotation changes the meaning of every coordinate in the write
 * path; threading a rotation flag through that file would leave two
 * interleaved addressing schemes in code shared with the ST7305. The register
 * programming below is deliberately the same sequence upstream uses, so the
 * panel is brought up identically - only the geometry differs.
 *
 * === Checked against Waveshare's own driver ===
 *
 * The packed output of st7306_rotate_strips() is byte-for-byte identical to
 * what Waveshare's ESP-IDF driver for this board produces, over a full frame.
 * That driver builds a lookup table (InitLandscapeLUT) mapping each landscape
 * pixel to a byte index and bit mask:
 *
 *     index = (x >> 1) * (height / 4) + ((height - 1 - y) >> 2)
 *     bit   = 7 - ((((height - 1 - y) & 3) << 1) | (x & 1))
 *
 * which is the same rotation and the same 4-column-by-2-row byte layout the
 * loop below writes directly. Its column window (0x2A: 0x12..0x2A) is also
 * where this driver's start-column of 216 comes from.
 */

#define DT_DRV_COMPAT waveshare_st7306_landscape

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/kernel.h>
#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(st7306_waveshare, CONFIG_DISPLAY_LOG_LEVEL);

/* Registers. Names and values follow the ST7306 datasheet section 8.1. */
#define ST7306_SLEEP_IN              0x10
#define ST7306_SLEEP_OUT             0x11
#define ST7306_SET_NORMAL_DISPLAY    0x20
#define ST7306_SET_REVERSE_DISPLAY   0x21
#define ST7306_DISPLAY_OFF           0x28
#define ST7306_DISPLAY_ON            0x29
#define ST7306_SET_COLUMN_ADDR       0x2A
#define ST7306_SET_ROW_ADDR          0x2B
#define ST7306_WRITE                 0x2C
#define ST7306_TEARING_OUT           0x35
#define ST7306_TEARING_OUT_VBLANK    0x00
#define ST7306_MADCTL                0x36
#define ST7306_HPM                   0x38
#define ST7306_LPM                   0x39
#define ST7306_DTFORM                0x3A
#define ST7306_DTFORM_3W_24B         0x11
#define ST7306_GATESET               0xB0
#define ST7306_FIRSTGATE             0xB1
#define ST7306_FRAMERATE             0xB2
#define ST7306_HPM_GATE_WAVEFORM     0xB3
#define ST7306_LPM_GATE_WAVEFORM     0xB4
#define ST7306_SOURCE_EQ_EN          0xB7
#define ST7306_SOURCE_EQ_EN_ENABLE   0x13
#define ST7306_PNLSET                0xB8
#define ST7306_GAMAMS                0xB9
#define ST7306_GAMAMS_MONO           0x20
#define ST7306_GATE_VOLTAGE          0xC0
#define ST7306_VSH                   0xC1
#define ST7306_VSL                   0xC2
#define ST7306_VSHN                  0xC4
#define ST7306_VSLN                  0xC5
#define ST7306_VSHLSEL               0xC9
#define ST7306_AUTOPWRDOWN           0xD0
#define ST7306_AUTOPWRDOWN_ON        0xFF
#define ST7306_BOOSTER_EN            0xD1
#define ST7306_BOOSTER_EN_ENABLE     0x01
#define ST7306_NVM_LOAD              0xD6
#define ST7306_OSC_SETTINGS          0xD8
#define ST7306_OSC_SETTINGS_BYTE2    0xE9

#define ST7306_HPM_GATE_WAVEFORM_LEN 10
#define ST7306_LPM_GATE_WAVEFORM_LEN 8

/*
 * Controller addressing granularity, in panel pixels.
 *
 * One column address covers 12 panel columns; one row address covers 2 panel
 * rows. A write window must start and end on these boundaries.
 */
#define ST7306_PIXELS_PER_COL_ADDR 12
#define ST7306_PIXELS_PER_ROW_ADDR 2

#define ST7306_RESET_DELAY 100
#define ST7306_SLEEP_DELAY 100

/*
 * Panel geometry, in panel (portrait) orientation. The driver presents the
 * transpose of this to the display API.
 */
#define PANEL_W DT_INST_PROP(0, panel_width)
#define PANEL_H DT_INST_PROP(0, panel_height)

/*
 * Landscape resolution reported to the application. The devicetree states
 * these too, as the node's width and height, so that code reading the chosen
 * display node sees landscape coordinates; the asserts below tie the two
 * descriptions together so they cannot drift apart.
 */
#define LANDSCAPE_W PANEL_H
#define LANDSCAPE_H PANEL_W

BUILD_ASSERT(DT_INST_PROP(0, width) == LANDSCAPE_W,
	     "Node width must be the transpose of panel-height");
BUILD_ASSERT(DT_INST_PROP(0, height) == LANDSCAPE_H,
	     "Node height must be the transpose of panel-width");

/*
 * The geometry this driver's addressing math depends on. These are checked
 * rather than assumed because every one of them being exact is the reason
 * landscape can use the full glass (see the header comment).
 */
BUILD_ASSERT(LANDSCAPE_W % 8 == 0,
	     "Landscape width must be a whole number of bytes in a packed 1bpp row");
BUILD_ASSERT(PANEL_W % ST7306_PIXELS_PER_COL_ADDR == 0,
	     "Panel width must be a multiple of the 12-pixel column address unit");
BUILD_ASSERT(PANEL_H % ST7306_PIXELS_PER_ROW_ADDR == 0,
	     "Panel height must be a multiple of the 2-pixel row address unit");

/*
 * Bytes the controller consumes for one row address, i.e. for a 2-pixel-tall
 * strip spanning the full panel width. Two panel rows at 1 bit per pixel.
 */
#define PANEL_STRIP_BYTES ((PANEL_W * ST7306_PIXELS_PER_ROW_ADDR) / 8)

struct st7306_config {
	const struct device *mipi_dev;
	const struct mipi_dbi_config dbi_config;
	uint16_t start_line;
	uint16_t start_column;
	uint8_t nvm_load[2];
	uint8_t gate_voltages[2];
	uint8_t vsh[4];
	uint8_t vsl[4];
	uint8_t vshn[4];
	uint8_t vsln[4];
	uint8_t osc_settings;
	uint8_t framerate;
	uint8_t multiplex_ratio;
	uint8_t source_voltage;
	uint8_t remap_value;
	uint8_t panel_settings;
	uint8_t hpm_gate_waveform[ST7306_HPM_GATE_WAVEFORM_LEN];
	uint8_t lpm_gate_waveform[ST7306_LPM_GATE_WAVEFORM_LEN];
	bool color_inversion;
	bool low_power_mode;
	uint8_t *conversion_buf;
	size_t conversion_buf_size;
};

struct st7306_data {
	bool blanking_on;
};

static int st7306_cmd(const struct device *dev, uint8_t cmd, const uint8_t *data, size_t len)
{
	const struct st7306_config *config = dev->config;

	return mipi_dbi_command_write(config->mipi_dev, &config->dbi_config, cmd, data, len);
}

static int st7306_resume(const struct device *dev)
{
	struct st7306_data *data = dev->data;
	const struct st7306_config *config = dev->config;
	int err;

	err = st7306_cmd(dev, ST7306_SLEEP_OUT, NULL, 0);
	if (err < 0) {
		return err;
	}
	k_msleep(ST7306_SLEEP_DELAY);

	err = st7306_cmd(dev, ST7306_DISPLAY_ON, NULL, 0);
	if (err < 0) {
		return err;
	}

	data->blanking_on = false;

	return mipi_dbi_release(config->mipi_dev, &config->dbi_config);
}

static int st7306_suspend(const struct device *dev)
{
	struct st7306_data *data = dev->data;
	const struct st7306_config *config = dev->config;
	int err;

	err = st7306_cmd(dev, ST7306_SLEEP_IN, NULL, 0);
	if (err < 0) {
		return err;
	}
	k_msleep(ST7306_SLEEP_DELAY);

	data->blanking_on = true;

	return mipi_dbi_release(config->mipi_dev, &config->dbi_config);
}

/*
 * Bring-up sequence.
 *
 * This mirrors the upstream st730x driver's ordering: the analog rails and
 * oscillator are programmed before the gate waveforms, and the panel is left
 * in whichever power mode the devicetree selected. Reordering these is not
 * safe - the booster has to be enabled before the voltages it generates are
 * set, and the gate waveforms are latched against the frame rate.
 */
static int st7306_configure(const struct device *dev)
{
	const struct st7306_config *config = dev->config;
	uint8_t tmp[2];
	int err;

	struct {
		uint8_t cmd;
		const uint8_t *data;
		size_t len;
	} const seq[] = {
		{ ST7306_NVM_LOAD, config->nvm_load, 2 },
		{ ST7306_BOOSTER_EN, (const uint8_t[]){ ST7306_BOOSTER_EN_ENABLE }, 1 },
		{ ST7306_GATE_VOLTAGE, config->gate_voltages, 2 },
		{ ST7306_VSH, config->vsh, 4 },
		{ ST7306_VSL, config->vsl, 4 },
		{ ST7306_VSHN, config->vshn, 4 },
		{ ST7306_VSLN, config->vsln, 4 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(seq); i++) {
		err = st7306_cmd(dev, seq[i].cmd, seq[i].data, seq[i].len);
		if (err < 0) {
			return err;
		}
	}

	tmp[0] = config->osc_settings;
	tmp[1] = ST7306_OSC_SETTINGS_BYTE2;
	err = st7306_cmd(dev, ST7306_OSC_SETTINGS, tmp, 2);
	if (err < 0) {
		return err;
	}

	err = st7306_cmd(dev, ST7306_FRAMERATE, &config->framerate, 1);
	if (err < 0) {
		return err;
	}

	err = st7306_cmd(dev, ST7306_HPM_GATE_WAVEFORM, config->hpm_gate_waveform,
			 ST7306_HPM_GATE_WAVEFORM_LEN);
	if (err < 0) {
		return err;
	}

	err = st7306_cmd(dev, ST7306_LPM_GATE_WAVEFORM, config->lpm_gate_waveform,
			 ST7306_LPM_GATE_WAVEFORM_LEN);
	if (err < 0) {
		return err;
	}

	tmp[0] = ST7306_SOURCE_EQ_EN_ENABLE;
	err = st7306_cmd(dev, ST7306_SOURCE_EQ_EN, tmp, 1);
	if (err < 0) {
		return err;
	}

	err = st7306_cmd(dev, ST7306_GATESET, &config->multiplex_ratio, 1);
	if (err < 0) {
		return err;
	}

	err = st7306_cmd(dev, ST7306_VSHLSEL, &config->source_voltage, 1);
	if (err < 0) {
		return err;
	}

	/*
	 * MADCTL. MV stays 0: the write path below emits data in the
	 * controller's native horizontal scan order and does the landscape
	 * transform itself, so the address counter must keep incrementing
	 * along X. The MX/MY/GS bits in remap-value still apply and are how
	 * the board's mirroring is expressed.
	 */
	err = st7306_cmd(dev, ST7306_MADCTL, &config->remap_value, 1);
	if (err < 0) {
		return err;
	}

	tmp[0] = ST7306_DTFORM_3W_24B;
	err = st7306_cmd(dev, ST7306_DTFORM, tmp, 1);
	if (err < 0) {
		return err;
	}

	tmp[0] = ST7306_GAMAMS_MONO;
	err = st7306_cmd(dev, ST7306_GAMAMS, tmp, 1);
	if (err < 0) {
		return err;
	}

	err = st7306_cmd(dev, ST7306_PNLSET, &config->panel_settings, 1);
	if (err < 0) {
		return err;
	}

	tmp[0] = ST7306_TEARING_OUT_VBLANK;
	err = st7306_cmd(dev, ST7306_TEARING_OUT, tmp, 1);
	if (err < 0) {
		return err;
	}

	tmp[0] = ST7306_AUTOPWRDOWN_ON;
	err = st7306_cmd(dev, ST7306_AUTOPWRDOWN, tmp, 1);
	if (err < 0) {
		return err;
	}

	tmp[0] = (config->start_line & 0x100) >> 8;
	tmp[1] = config->start_line & 0xFF;
	err = st7306_cmd(dev, ST7306_FIRSTGATE, tmp, 2);
	if (err < 0) {
		return err;
	}

	return st7306_cmd(dev, config->low_power_mode ? ST7306_LPM : ST7306_HPM, NULL, 0);
}

/*
 * One write window, in both coordinate systems.
 *
 * The caller works in landscape coordinates; the controller is addressed in
 * panel ones. Resolving both once keeps the rotation loop free of the
 * conversion and gives st7306_write() a single place to validate alignment.
 *
 *   lx1..lx2   landscape X span, which maps to panel ROWS
 *   ly1        first landscape row, which maps to the LAST panel column
 *   lw, lh     landscape width and height of the band
 *   px1        first panel column the band covers
 */
struct st7306_window {
	uint32_t lx1;
	uint32_t lx2;
	uint32_t ly1;
	uint32_t lw;
	uint32_t lh;
	uint32_t px1;
};

/*
 * Rotate one horizontal band of the landscape framebuffer into the
 * controller's native layout.
 *
 * Coordinates: (lx, ly) are landscape pixels, (px, py) are panel pixels. The
 * image is rotated 90 degrees clockwise onto the glass:
 *
 *     px = (LANDSCAPE_H - 1) - ly
 *     py = lx
 *
 * so a landscape row runs down a panel column, and successive landscape rows
 * walk right-to-left across the panel. Landscape (0,0) lands at the panel's
 * top-right corner, which means the board is read with its portrait "top" edge
 * on the viewer's left.
 *
 * If the board is mounted the other way round, flip both axes by swapping the
 * two expressions above for
 *
 *     px = ly
 *     py = (LANDSCAPE_W - 1) - lx
 *
 * which is the 90-degree counter-clockwise rotation; nothing else in the
 * driver changes, since the addressing is symmetric.
 *
 * Feeding the panel in its own scan order means iterating the destination and
 * gathering from the source, which is why this reads source pixels one at a
 * time rather than streaming them.
 *
 * The destination byte layout is the ST730x "evil" format: each byte holds a
 * 4-wide by 2-tall block of pixels, ordered
 *
 *     p1 p3 p5 p7      <- MSB first, top row
 *     p2 p4 p6 p8         bottom row
 *
 * i.e. bit 7 is the top-left pixel of the block, bit 6 the one directly below
 * it, bit 5 the next column's top pixel, and so on. Two panel rows are
 * therefore produced together, which is exactly one row address.
 *
 * Returns the number of panel row-address strips written into the conversion
 * buffer, i.e. how many pairs of panel rows are ready to send.
 */
static uint32_t st7306_rotate_strips(const struct device *dev, const uint8_t *src,
				     const struct st7306_window *win, uint32_t first_strip,
				     uint32_t max_strips)
{
	const struct st7306_config *config = dev->config;
	/*
	 * Source rows are the caller's band, not the whole screen, so the
	 * stride is the band's width rather than LANDSCAPE_W.
	 */
	const uint32_t src_stride = win->lw / 8;
	const uint32_t strip_bytes = (win->lh * ST7306_PIXELS_PER_ROW_ADDR) / 8;
	uint32_t strips = 0;

	for (; strips < max_strips; strips++) {
		const uint32_t py = win->lx1 + (first_strip + strips) * ST7306_PIXELS_PER_ROW_ADDR;
		uint8_t *dst = &config->conversion_buf[strips * strip_bytes];

		if (py > win->lx2) {
			break;
		}

		/*
		 * py and py + 1 are two adjacent panel rows, which under the
		 * mapping above are two adjacent landscape columns: lx = py
		 * and lx = py + 1. Walking px across the panel walks ly back
		 * up the landscape image.
		 */
		const uint32_t lx_top = py - win->lx1;
		const uint32_t lx_bot = lx_top + 1;
		const uint8_t top_mask = 0x80U >> (lx_top % 8);
		const uint8_t bot_mask = 0x80U >> (lx_bot % 8);
		const uint32_t top_byte = lx_top / 8;
		const uint32_t bot_byte = lx_bot / 8;

		memset(dst, 0, strip_bytes);

		/*
		 * i walks the band's own column addresses, and the rotation
		 * makes the band's LAST landscape row the first column the
		 * controller consumes - hence the descending read.
		 *
		 * Band-local throughout: `src` holds only this band, so the
		 * index is an offset within it and never refers to absolute
		 * landscape coordinates. Where the band lands on the glass is
		 * decided once, by win.px1 and the column address window, not
		 * here.
		 */
		for (uint32_t i = 0; i < win->lh; i++) {
			const uint8_t *row = &src[(win->lh - 1 - i) * src_stride];
			uint8_t bits = 0;

			/*
			 * Bit placement within the byte.
			 *
			 * One byte holds a 4-wide by 2-tall block of panel
			 * pixels. The leftmost of the four columns takes the
			 * highest bit pair, and within a pair the upper panel
			 * row is the odd (higher) bit:
			 *
			 *     bit  7 6 | 5 4 | 3 2 | 1 0
			 *          c0  |  c1 |  c2 |  c3   <- panel columns
			 *         t b  | t b | t b | t b
			 *
			 * This matches Waveshare's own driver, whose landscape
			 * lookup table computes the same block as
			 * bit = 7 - ((px & 3) << 1 | (py & 1)).
			 *
			 * Getting this order backwards mirrors the image
			 * within each 4-pixel block, which on the glass reads
			 * as a skew across the width rather than as an obvious
			 * flip.
			 */
			const uint32_t shift = 6U - 2U * (i % 4);

			if (row[top_byte] & top_mask) {
				bits |= 0x2;
			}
			if (row[bot_byte] & bot_mask) {
				bits |= 0x1;
			}

			dst[i / 4] |= bits << shift;
		}
	}

	return strips;
}

static int st7306_write(const struct device *dev, const uint16_t x, const uint16_t y,
			const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct st7306_config *config = dev->config;
	const uint8_t *src = buf;
	uint32_t strip = 0;
	uint32_t total_strips;
	int err;

	if (desc->height == 0 || desc->width == 0) {
		return 0;
	}

	if (buf == NULL) {
		LOG_ERR("Display buffer is not available");
		return -EINVAL;
	}

	if (desc->pitch != desc->width) {
		LOG_ERR("Pitch (%u) must equal width (%u)", desc->pitch, desc->width);
		return -EINVAL;
	}

	/*
	 * Alignment, in landscape terms.
	 *
	 * The rotation maps a landscape X span onto panel rows and a landscape
	 * Y span onto panel columns, so the controller's two addressing units
	 * land on different axes:
	 *
	 *   landscape X -> panel rows, 2 pixels per row address
	 *   landscape Y -> panel columns, 12 pixels per column address
	 *
	 * Since LANDSCAPE_H is itself a multiple of 12, the Y condition works
	 * out to y starting on a multiple of 12 and the span ending one short
	 * of one. The application's rounder produces exactly this; anything
	 * else is a bug there rather than something to paper over here.
	 */
	/*
	 * X must be a whole number of bytes, not merely a multiple of the
	 * controller's 2-pixel row unit.
	 *
	 * A landscape X span is the packed axis of the incoming 1bpp band:
	 * Zephyr's mono glue writes it with `y * width / 8` and the gather
	 * below reads it back the same way, so a width that is not a multiple
	 * of 8 truncates the row stride and every row after the first comes
	 * from the wrong offset. The window would still be addressed
	 * correctly, so it shows up as garbage inside a correctly placed
	 * band rather than as a misplaced one.
	 */
	if (x % 8 != 0 || desc->width % 8 != 0) {
		LOG_ERR("X span %u..%u must align to 8 (whole bytes)", x, x + desc->width - 1);
		return -EINVAL;
	}

	BUILD_ASSERT(8 % ST7306_PIXELS_PER_ROW_ADDR == 0,
		     "Byte alignment must also satisfy the row address unit");

	if (y % ST7306_PIXELS_PER_COL_ADDR != 0 ||
	    desc->height % ST7306_PIXELS_PER_COL_ADDR != 0) {
		LOG_ERR("Y span %u..%u must align to %u", y, y + desc->height - 1,
			ST7306_PIXELS_PER_COL_ADDR);
		return -EINVAL;
	}

	if (x + desc->width > LANDSCAPE_W || y + desc->height > LANDSCAPE_H) {
		LOG_ERR("Window %ux%u at %u,%u exceeds %ux%u", desc->width, desc->height, x, y,
			LANDSCAPE_W, LANDSCAPE_H);
		return -EINVAL;
	}

	if (desc->buf_size < (desc->width * desc->height) / 8) {
		LOG_ERR("Display buffer too small (%u)", desc->buf_size);
		return -EINVAL;
	}

	const struct st7306_window win = {
		.lx1 = x,
		.lx2 = x + desc->width - 1,
		.ly1 = y,
		.lw = desc->width,
		.lh = desc->height,
		/*
		 * Where the band sits in the controller's column space.
		 *
		 * Not (LANDSCAPE_H - 1) - (y + height - 1), which is the
		 * unmirrored position. MADCTL bit 6 (MX) is set in
		 * remap-value, so the controller scans the column address
		 * window in the opposite direction and a window programmed at
		 * address A appears mirrored about the full glass span. The
		 * address to program is therefore the band's own landscape
		 * offset.
		 *
		 * A full-screen window is symmetric about that span, so both
		 * expressions give 0 for it and the whole-frame path cannot
		 * tell them apart. That is why this only ever showed up once
		 * partial bands existed, as redraws mirrored in y against a
		 * correct background.
		 */
		.px1 = y,
	};

	/*
	 * Bytes one row address consumes for this band: a strip is 2 panel
	 * rows wide in the panel's own terms, spanning the band's panel
	 * columns, which is its landscape height.
	 */
	const uint32_t strip_bytes = (win.lh * ST7306_PIXELS_PER_ROW_ADDR) / 8;

	/*
	 * Address only the band. Column addresses count panel columns from the
	 * glass origin, so the devicetree's start-column offset still applies.
	 */
	const uint8_t col_addrs[] = {
		(config->start_column + win.px1) / ST7306_PIXELS_PER_COL_ADDR,
		(config->start_column + win.px1 + win.lh) / ST7306_PIXELS_PER_COL_ADDR - 1,
	};
	const uint8_t row_addrs[] = {
		win.lx1 / ST7306_PIXELS_PER_ROW_ADDR,
		(win.lx2 + 1) / ST7306_PIXELS_PER_ROW_ADDR - 1,
	};

	err = st7306_cmd(dev, ST7306_SET_COLUMN_ADDR, col_addrs, 2);
	if (err < 0) {
		return err;
	}

	err = st7306_cmd(dev, ST7306_SET_ROW_ADDR, row_addrs, 2);
	if (err < 0) {
		return err;
	}

	err = st7306_cmd(dev, ST7306_WRITE, NULL, 0);
	if (err < 0) {
		return err;
	}

	/* Panel rows this band covers, i.e. its landscape width. */
	total_strips = win.lw / ST7306_PIXELS_PER_ROW_ADDR;

	/*
	 * Convert and send in chunks that fit the conversion buffer, so the
	 * driver never needs a second full framebuffer.
	 */
	while (strip < total_strips) {
		const uint32_t max_strips = config->conversion_buf_size / strip_bytes;
		struct display_buffer_descriptor mipi_desc = { 0 };
		uint32_t done;

		if (max_strips == 0) {
			LOG_ERR("Conversion buffer too small for a %u-pixel-tall band", win.lh);
			return -ENOMEM;
		}

		done = st7306_rotate_strips(dev, src, &win, strip, max_strips);
		if (done == 0) {
			LOG_ERR("Conversion made no progress");
			return -EIO;
		}

		strip += done;

		/*
		 * The descriptor describes the data as the panel sees it: a
		 * block win.lh panel columns wide and (done * 2) panel rows
		 * tall.
		 */
		mipi_desc.buf_size = done * strip_bytes;
		mipi_desc.width = win.lh;
		mipi_desc.height = done * ST7306_PIXELS_PER_ROW_ADDR;
		mipi_desc.pitch = win.lh;
		mipi_desc.frame_incomplete = (strip < total_strips);

		err = mipi_dbi_write_display(config->mipi_dev, &config->dbi_config,
					     config->conversion_buf, &mipi_desc,
					     PIXEL_FORMAT_MONO01);
		if (err < 0) {
			return err;
		}
	}

	return mipi_dbi_release(config->mipi_dev, &config->dbi_config);
}

static void st7306_get_capabilities(const struct device *dev, struct display_capabilities *caps)
{
	ARG_UNUSED(dev);

	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = LANDSCAPE_W;
	caps->y_resolution = LANDSCAPE_H;
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO01;
	caps->current_pixel_format = PIXEL_FORMAT_MONO01;

	/*
	 * No SCREEN_INFO_X_ALIGNMENT_WIDTH here, deliberately.
	 *
	 * That flag makes lvgl_rounder_cb_mono() widen every invalidated area
	 * to the full display width, which combined with a full-refresh buffer
	 * meant every flush was the whole 400x300 screen - 120000 pixels
	 * repacked by the glue and rotated by this driver, whatever had
	 * actually changed.
	 *
	 * The alignment this controller needs cannot be expressed through that
	 * interface anyway. Zephyr's rounder aligns with power-of-two bitmasks,
	 * and after the 90-degree rotation the real constraint is that a
	 * landscape Y span must align to the 12-pixel column-address unit,
	 * which is not a power of two. The application supplies its own rounder
	 * instead (see ui_round_area() and its use in main.c), and this driver
	 * validates what arrives.
	 */

	caps->screen_info = SCREEN_INFO_MONO_MSB_FIRST;
}

static int st7306_set_pixel_format(const struct device *dev, const enum display_pixel_format pf)
{
	ARG_UNUSED(dev);

	if (pf == PIXEL_FORMAT_MONO01) {
		return 0;
	}

	LOG_ERR("Unsupported pixel format (only MONO01)");

	return -ENOTSUP;
}

static int st7306_init(const struct device *dev)
{
	const struct st7306_config *config = dev->config;
	int err;

	if (!device_is_ready(config->mipi_dev)) {
		LOG_ERR("MIPI DBI device not ready");
		return -ENODEV;
	}

	err = mipi_dbi_reset(config->mipi_dev, ST7306_RESET_DELAY);
	if (err < 0) {
		LOG_ERR("Failed to reset display (%d)", err);
		return err;
	}

	/*
	 * The controller is configured while asleep, matching the datasheet's
	 * power-on flow, then woken once the rails and waveforms are set.
	 */
	err = st7306_suspend(dev);
	if (err < 0) {
		return err;
	}

	err = st7306_configure(dev);
	if (err < 0) {
		LOG_ERR("Failed to configure display (%d)", err);
		return err;
	}

	err = st7306_cmd(dev, config->color_inversion ? ST7306_SET_REVERSE_DISPLAY
						      : ST7306_SET_NORMAL_DISPLAY,
			 NULL, 0);
	if (err < 0) {
		return err;
	}

	err = st7306_resume(dev);
	if (err < 0) {
		return err;
	}

	LOG_INF("ST7306 ready: %ux%u landscape on %ux%u panel", LANDSCAPE_W, LANDSCAPE_H, PANEL_W,
		PANEL_H);

	return mipi_dbi_release(config->mipi_dev, &config->dbi_config);
}

static DEVICE_API(display, st7306_api) = {
	.blanking_on = st7306_suspend,
	.blanking_off = st7306_resume,
	.write = st7306_write,
	.get_capabilities = st7306_get_capabilities,
	.set_pixel_format = st7306_set_pixel_format,
};

/*
 * Conversion buffer, sized in whole panel row-address strips so the chunking
 * loop never splits one.
 */
static uint8_t st7306_conv_buf[CONFIG_ST7306_WAVESHARE_CONV_BUFFER_STRIPS * PANEL_STRIP_BYTES];

static struct st7306_data st7306_data_0;

static const struct st7306_config st7306_config_0 = {
	.mipi_dev = DEVICE_DT_GET(DT_INST_PARENT(0)),
	.dbi_config = MIPI_DBI_CONFIG_DT_INST(0, SPI_WORD_SET(8) | SPI_OP_MODE_MASTER, 0),
	.start_line = DT_INST_PROP(0, start_line),
	.start_column = DT_INST_PROP(0, start_column),
	.nvm_load = DT_INST_PROP(0, nvm_load),
	.gate_voltages = DT_INST_PROP(0, gate_voltages),
	.vsh = DT_INST_PROP(0, vsh),
	.vsl = DT_INST_PROP(0, vsl),
	.vshn = DT_INST_PROP(0, vshn),
	.vsln = DT_INST_PROP(0, vsln),
	.osc_settings = DT_INST_PROP(0, osc_settings),
	.framerate = DT_INST_PROP(0, framerate),
	.multiplex_ratio = DT_INST_PROP(0, multiplex_ratio),
	.source_voltage = DT_INST_PROP(0, source_voltage),
	.remap_value = DT_INST_PROP(0, remap_value),
	.panel_settings = DT_INST_PROP(0, panel_settings),
	.hpm_gate_waveform = DT_INST_PROP(0, hpm_gate_waveform),
	.lpm_gate_waveform = DT_INST_PROP(0, lpm_gate_waveform),
	.color_inversion = DT_INST_PROP(0, inversion_on),
	.low_power_mode = DT_INST_PROP(0, low_power_mode),
	.conversion_buf = st7306_conv_buf,
	.conversion_buf_size = sizeof(st7306_conv_buf),
};

DEVICE_DT_INST_DEFINE(0, st7306_init, NULL, &st7306_data_0, &st7306_config_0, POST_KERNEL,
		      CONFIG_DISPLAY_INIT_PRIORITY, &st7306_api);
