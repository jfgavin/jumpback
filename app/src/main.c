/*
 * Copyright (c) 2026 Josh Gav
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <lvgl.h>

#include "tacho.h"
#include "ui.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define FRAME_INTERVAL_MS     20
#define RPM_STEP              300
#define HEARTBEAT_INTERVAL_MS 1000
#define FRAMES_PER_HEARTBEAT  (HEARTBEAT_INTERVAL_MS / FRAME_INTERVAL_MS)

#define MODEL_NAME      "JMP-1"
#define MODEL_NAME_FONT lv_font_montserrat_28

static void draw_model_name(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_draw_label_dsc_t dsc;
	const lv_area_t area = {
		.x1 = UI_MARGIN,
		.y1 = UI_MARGIN,
		.x2 = UI_SCREEN_W - UI_MARGIN,
		.y2 = UI_SCREEN_H - UI_MARGIN,
	};

	lv_draw_label_dsc_init(&dsc);
	dsc.font = &MODEL_NAME_FONT;
	dsc.color = lv_color_hex(UI_COLOR_DIM);
	dsc.text = MODEL_NAME;

	lv_draw_label(layer, &dsc, &area);
}

int main(void)
{
	const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	static struct tacho tacho;
	static uint8_t tacho_buf[TACHO_BUF_SIZE(UI_GAUGE_W, UI_GAUGE_H)];
	lv_obj_t *screen;
	int err;

	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device not ready");
		return 0;
	}

	screen = lv_screen_active();
	lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), LV_PART_MAIN);
	lv_obj_add_event_cb(screen, draw_model_name, LV_EVENT_DRAW_MAIN_END, NULL);
	lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

	lv_timer_handler();
	display_blanking_off(display_dev);

	err = tacho_init(&tacho, screen, NULL, tacho_buf, sizeof(tacho_buf));
	if (err) {
		LOG_ERR("Tacho init failed (%d)", err);
		return 0;
	}

	const int32_t rpm_max = tacho_get_rpm_max(&tacho);
	int32_t rpm = 0;
	uint32_t frame = 0;

	while (true) {
		tacho_set_rpm(&tacho, rpm);

		rpm = (rpm + RPM_STEP > rpm_max) ? 0 : rpm + RPM_STEP;

		if (++frame % FRAMES_PER_HEARTBEAT == 0) {
			LOG_INF("alive: rpm=%d", rpm);
		}

		lv_timer_handler();
		k_msleep(FRAME_INTERVAL_MS);
	}

	return 0;
}
