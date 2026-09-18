/*
 * Copyright (c) 2026 Josh Gav
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT waveshare_st7306_landscape

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include <dt-bindings/display/st7306.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(st7306_waveshare, CONFIG_DISPLAY_LOG_LEVEL);

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

#define ST7306_NVM_LOAD_LEN          2
#define ST7306_GATE_VOLTAGES_LEN     2
#define ST7306_SOURCE_VOLTAGE_MODES  4
#define ST7306_HPM_GATE_WAVEFORM_LEN 10
#define ST7306_LPM_GATE_WAVEFORM_LEN 8

#define ST7306_RESET_DELAY_MS 100
#define ST7306_SLEEP_DELAY_MS 100

#define NATIVE_BLOCK_COLUMNS            4
#define NATIVE_BLOCK_ROWS               ST7306_PIXELS_PER_ROW_ADDR
#define NATIVE_BLOCK_FIRST_COLUMN_SHIFT ((NATIVE_BLOCK_COLUMNS - 1) * NATIVE_BLOCK_ROWS)
#define NATIVE_BLOCK_TOP_ROW_BIT        BIT(1)
#define NATIVE_BLOCK_BOTTOM_ROW_BIT     BIT(0)

#define MONO_MSB_FIRST_MASK(x) (BIT(BITS_PER_BYTE - 1) >> ((x) % BITS_PER_BYTE))

#define PANEL_W     DT_INST_PROP(0, panel_width)
#define PANEL_H     DT_INST_PROP(0, panel_height)
#define LANDSCAPE_W PANEL_H
#define LANDSCAPE_H PANEL_W
#define REMAP_VALUE DT_INST_PROP(0, remap_value)

#define PANEL_STRIP_BYTES ((PANEL_W * ST7306_PIXELS_PER_ROW_ADDR) / BITS_PER_BYTE)

BUILD_ASSERT(DT_INST_PROP(0, width) == LANDSCAPE_W,
	     "Node width must be the transpose of panel-height");
BUILD_ASSERT(DT_INST_PROP(0, height) == LANDSCAPE_H,
	     "Node height must be the transpose of panel-width");
BUILD_ASSERT(LANDSCAPE_W % BITS_PER_BYTE == 0,
	     "Landscape width must be a whole number of bytes in a packed 1bpp row");
BUILD_ASSERT(PANEL_W % ST7306_PIXELS_PER_COL_ADDR == 0,
	     "Panel width must be a multiple of the column address unit");
BUILD_ASSERT(PANEL_H % ST7306_PIXELS_PER_ROW_ADDR == 0,
	     "Panel height must be a multiple of the row address unit");
BUILD_ASSERT(BITS_PER_BYTE % ST7306_PIXELS_PER_ROW_ADDR == 0,
	     "Byte alignment must also satisfy the row address unit");
BUILD_ASSERT((REMAP_VALUE & ST7306_MADCTL_MV) == 0,
	     "MV must stay clear: rotation is done in software, in horizontal scan order");
BUILD_ASSERT((REMAP_VALUE & ST7306_MADCTL_MX) != 0,
	     "Band placement relies on MX mirroring the column scan");

struct st7306_config {
	const struct device *mipi_dev;
	const struct mipi_dbi_config dbi_config;
	uint16_t start_line;
	uint16_t start_column;
	uint8_t nvm_load[ST7306_NVM_LOAD_LEN];
	uint8_t gate_voltages[ST7306_GATE_VOLTAGES_LEN];
	uint8_t vsh[ST7306_SOURCE_VOLTAGE_MODES];
	uint8_t vsl[ST7306_SOURCE_VOLTAGE_MODES];
	uint8_t vshn[ST7306_SOURCE_VOLTAGE_MODES];
	uint8_t vsln[ST7306_SOURCE_VOLTAGE_MODES];
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

struct st7306_command {
	uint8_t cmd;
	const uint8_t *data;
	size_t len;
};

struct st7306_band {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
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
	k_msleep(ST7306_SLEEP_DELAY_MS);

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
	k_msleep(ST7306_SLEEP_DELAY_MS);

	data->blanking_on = true;

	return mipi_dbi_release(config->mipi_dev, &config->dbi_config);
}

static int st7306_configure(const struct device *dev)
{
	const struct st7306_config *config = dev->config;
	const uint8_t booster_enable[] = { ST7306_BOOSTER_EN_ENABLE };
	const uint8_t osc_settings[] = { config->osc_settings, ST7306_OSC_SETTINGS_BYTE2 };
	const uint8_t source_eq_enable[] = { ST7306_SOURCE_EQ_EN_ENABLE };
	const uint8_t data_format[] = { ST7306_DTFORM_3W_24B };
	const uint8_t gamma_mode[] = { ST7306_GAMAMS_MONO };
	const uint8_t tearing_out[] = { ST7306_TEARING_OUT_VBLANK };
	const uint8_t auto_power_down[] = { ST7306_AUTOPWRDOWN_ON };
	uint8_t first_gate[sizeof(uint16_t)];

	sys_put_be16(config->start_line, first_gate);

	const struct st7306_command sequence[] = {
		{ ST7306_NVM_LOAD, config->nvm_load, sizeof(config->nvm_load) },
		{ ST7306_BOOSTER_EN, booster_enable, sizeof(booster_enable) },
		{ ST7306_GATE_VOLTAGE, config->gate_voltages, sizeof(config->gate_voltages) },
		{ ST7306_VSH, config->vsh, sizeof(config->vsh) },
		{ ST7306_VSL, config->vsl, sizeof(config->vsl) },
		{ ST7306_VSHN, config->vshn, sizeof(config->vshn) },
		{ ST7306_VSLN, config->vsln, sizeof(config->vsln) },
		{ ST7306_OSC_SETTINGS, osc_settings, sizeof(osc_settings) },
		{ ST7306_FRAMERATE, &config->framerate, sizeof(config->framerate) },
		{ ST7306_HPM_GATE_WAVEFORM, config->hpm_gate_waveform,
		  sizeof(config->hpm_gate_waveform) },
		{ ST7306_LPM_GATE_WAVEFORM, config->lpm_gate_waveform,
		  sizeof(config->lpm_gate_waveform) },
		{ ST7306_SOURCE_EQ_EN, source_eq_enable, sizeof(source_eq_enable) },
		{ ST7306_GATESET, &config->multiplex_ratio, sizeof(config->multiplex_ratio) },
		{ ST7306_VSHLSEL, &config->source_voltage, sizeof(config->source_voltage) },
		{ ST7306_MADCTL, &config->remap_value, sizeof(config->remap_value) },
		{ ST7306_DTFORM, data_format, sizeof(data_format) },
		{ ST7306_GAMAMS, gamma_mode, sizeof(gamma_mode) },
		{ ST7306_PNLSET, &config->panel_settings, sizeof(config->panel_settings) },
		{ ST7306_TEARING_OUT, tearing_out, sizeof(tearing_out) },
		{ ST7306_AUTOPWRDOWN, auto_power_down, sizeof(auto_power_down) },
		{ ST7306_FIRSTGATE, first_gate, sizeof(first_gate) },
		{ config->low_power_mode ? ST7306_LPM : ST7306_HPM, NULL, 0 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(sequence); i++) {
		int err = st7306_cmd(dev, sequence[i].cmd, sequence[i].data, sequence[i].len);

		if (err < 0) {
			return err;
		}
	}

	return 0;
}

static uint32_t st7306_strip_bytes(const struct st7306_band *band)
{
	return (band->height * ST7306_PIXELS_PER_ROW_ADDR) / BITS_PER_BYTE;
}

static uint32_t st7306_band_strips(const struct st7306_band *band)
{
	return band->width / ST7306_PIXELS_PER_ROW_ADDR;
}

static uint32_t st7306_rotate_strips(const struct st7306_config *config,
				     const uint8_t *landscape_band,
				     const struct st7306_band *band, uint32_t first_strip,
				     uint32_t max_strips)
{
	const uint32_t landscape_stride = band->width / BITS_PER_BYTE;
	const uint32_t strip_bytes = st7306_strip_bytes(band);
	const uint32_t band_strips = st7306_band_strips(band);
	uint32_t strips = 0;

	for (; strips < max_strips && first_strip + strips < band_strips; strips++) {
		const uint32_t top_x = (first_strip + strips) * ST7306_PIXELS_PER_ROW_ADDR;
		const uint32_t bottom_x = top_x + 1;
		const uint32_t top_byte = top_x / BITS_PER_BYTE;
		const uint32_t bottom_byte = bottom_x / BITS_PER_BYTE;
		const uint8_t top_mask = MONO_MSB_FIRST_MASK(top_x);
		const uint8_t bottom_mask = MONO_MSB_FIRST_MASK(bottom_x);
		uint8_t *strip = &config->conversion_buf[strips * strip_bytes];

		memset(strip, 0, strip_bytes);

		for (uint32_t panel_column = 0; panel_column < band->height; panel_column++) {
			const uint32_t landscape_y = band->height - 1 - panel_column;
			const uint8_t *landscape_row = &landscape_band[landscape_y * landscape_stride];
			const uint32_t shift =
				NATIVE_BLOCK_FIRST_COLUMN_SHIFT -
				NATIVE_BLOCK_ROWS * (panel_column % NATIVE_BLOCK_COLUMNS);
			uint8_t column_pair = 0;

			if (landscape_row[top_byte] & top_mask) {
				column_pair |= NATIVE_BLOCK_TOP_ROW_BIT;
			}
			if (landscape_row[bottom_byte] & bottom_mask) {
				column_pair |= NATIVE_BLOCK_BOTTOM_ROW_BIT;
			}

			strip[panel_column / NATIVE_BLOCK_COLUMNS] |= column_pair << shift;
		}
	}

	return strips;
}

static int st7306_check_band(const struct st7306_band *band,
			     const struct display_buffer_descriptor *desc)
{
	if (desc->pitch != desc->width) {
		LOG_ERR("Pitch (%u) must equal width (%u)", desc->pitch, desc->width);
		return -EINVAL;
	}

	if (band->x % BITS_PER_BYTE != 0 || band->width % BITS_PER_BYTE != 0) {
		LOG_ERR("X span %u..%u must align to %u (whole bytes)", band->x,
			band->x + band->width - 1, BITS_PER_BYTE);
		return -EINVAL;
	}

	if (band->y % ST7306_PIXELS_PER_COL_ADDR != 0 ||
	    band->height % ST7306_PIXELS_PER_COL_ADDR != 0) {
		LOG_ERR("Y span %u..%u must align to %u", band->y, band->y + band->height - 1,
			ST7306_PIXELS_PER_COL_ADDR);
		return -EINVAL;
	}

	if (band->x + band->width > LANDSCAPE_W || band->y + band->height > LANDSCAPE_H) {
		LOG_ERR("Window %ux%u at %u,%u exceeds %ux%u", band->width, band->height, band->x,
			band->y, LANDSCAPE_W, LANDSCAPE_H);
		return -EINVAL;
	}

	if (desc->buf_size < (band->width * band->height) / BITS_PER_BYTE) {
		LOG_ERR("Display buffer too small (%u)", desc->buf_size);
		return -EINVAL;
	}

	return 0;
}

static int st7306_write(const struct device *dev, const uint16_t x, const uint16_t y,
			const struct display_buffer_descriptor *desc, const void *buf)
{
	const struct st7306_config *config = dev->config;
	const struct st7306_band band = {
		.x = x,
		.y = y,
		.width = desc->width,
		.height = desc->height,
	};
	int err;

	if (band.width == 0 || band.height == 0) {
		return 0;
	}

	if (buf == NULL) {
		LOG_ERR("Display buffer is not available");
		return -EINVAL;
	}

	err = st7306_check_band(&band, desc);
	if (err < 0) {
		return err;
	}

	const uint32_t strip_bytes = st7306_strip_bytes(&band);
	const uint32_t band_strips = st7306_band_strips(&band);
	const uint32_t strips_per_chunk = config->conversion_buf_size / strip_bytes;

	if (strips_per_chunk == 0) {
		LOG_ERR("Conversion buffer too small for a %u-pixel-tall band", band.height);
		return -ENOMEM;
	}

	const uint32_t first_panel_column = config->start_column + band.y;
	const uint8_t column_addresses[] = {
		first_panel_column / ST7306_PIXELS_PER_COL_ADDR,
		(first_panel_column + band.height) / ST7306_PIXELS_PER_COL_ADDR - 1,
	};
	const uint8_t row_addresses[] = {
		band.x / ST7306_PIXELS_PER_ROW_ADDR,
		(band.x + band.width) / ST7306_PIXELS_PER_ROW_ADDR - 1,
	};

	err = st7306_cmd(dev, ST7306_SET_COLUMN_ADDR, column_addresses, sizeof(column_addresses));
	if (err < 0) {
		return err;
	}

	err = st7306_cmd(dev, ST7306_SET_ROW_ADDR, row_addresses, sizeof(row_addresses));
	if (err < 0) {
		return err;
	}

	err = st7306_cmd(dev, ST7306_WRITE, NULL, 0);
	if (err < 0) {
		return err;
	}

	for (uint32_t strip = 0; strip < band_strips;) {
		const uint32_t converted =
			st7306_rotate_strips(config, buf, &band, strip, strips_per_chunk);

		strip += converted;

		struct display_buffer_descriptor chunk = {
			.buf_size = converted * strip_bytes,
			.width = band.height,
			.height = converted * ST7306_PIXELS_PER_ROW_ADDR,
			.pitch = band.height,
			.frame_incomplete = strip < band_strips,
		};

		err = mipi_dbi_write_display(config->mipi_dev, &config->dbi_config,
					     config->conversion_buf, &chunk, PIXEL_FORMAT_MONO01);
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

	err = mipi_dbi_reset(config->mipi_dev, ST7306_RESET_DELAY_MS);
	if (err < 0) {
		LOG_ERR("Failed to reset display (%d)", err);
		return err;
	}

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

static uint8_t st7306_conv_buf[CONFIG_ST7306_WAVESHARE_CONV_BUFFER_STRIPS * PANEL_STRIP_BYTES];

static struct st7306_data st7306_data_0;

static const struct st7306_config st7306_config_0 = {
	.mipi_dev = DEVICE_DT_GET(DT_INST_PARENT(0)),
	.dbi_config = MIPI_DBI_CONFIG_DT_INST(0, SPI_WORD_SET(8) | SPI_OP_MODE_CONTROLLER, 0),
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
	.remap_value = REMAP_VALUE,
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
