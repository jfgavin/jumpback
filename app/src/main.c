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
#define SWEEP_PERIOD_MS       3000
#define HEARTBEAT_INTERVAL_MS 1000

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

static int32_t sweep_rpm(int64_t elapsed_ms, int32_t rpm_max)
{
	const int32_t phase = (int32_t)(elapsed_ms % SWEEP_PERIOD_MS);

	return (int32_t)(((int64_t)phase * rpm_max) / SWEEP_PERIOD_MS);
}

static void sleep_until_next_frame(int64_t *next_frame)
{
	const int64_t now = k_uptime_get();

	*next_frame += FRAME_INTERVAL_MS;

	if (*next_frame <= now) {
		*next_frame = now + FRAME_INTERVAL_MS;
	} else {
		k_msleep((int32_t)(*next_frame - now));
	}
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
	const int64_t start = k_uptime_get();
	int64_t next_frame = start;
	int64_t last_heartbeat = start;

	while (true) {
		const int32_t rpm = sweep_rpm(k_uptime_get() - start, rpm_max);

		tacho_set_rpm(&tacho, rpm);

		if (k_uptime_get() - last_heartbeat >= HEARTBEAT_INTERVAL_MS) {
			last_heartbeat = k_uptime_get();
			LOG_INF("alive: rpm=%d", rpm);
		}

		lv_timer_handler();

		sleep_until_next_frame(&next_frame);
	}

	return 0;
}
