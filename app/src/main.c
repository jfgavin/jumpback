/*
 * Copyright (c) 2026 Josh Gav
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <lvgl.h>

#include "tacho.h"
#include "ui.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define FRAME_INTERVAL_MS     20
#define SWEEP_PERIOD_MS       3000
#define HEARTBEAT_INTERVAL_MS 1000

#define TENTHS_PER_UNIT 10U

#define DISPLAY_IS_ST7306_LANDSCAPE                                                                \
	DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_display), waveshare_st7306_landscape)

struct frame_stats {
	uint32_t frames;
	int64_t render_ms;
};

#if DISPLAY_IS_ST7306_LANDSCAPE
static bool covers_screen(const lv_area_t *area)
{
	return lv_area_get_width(area) >= UI_SCREEN_W && lv_area_get_height(area) >= UI_SCREEN_H;
}

static void round_area_to_panel_units(lv_event_t *e)
{
	lv_area_t *area = lv_event_get_param(e);
	lv_area_t tacho_span;
	const bool tacho_span_known = tacho_dirty_span(&tacho_span);

	if (tacho_span_known && covers_screen(area)) {
		*area = tacho_span;
	}

	area->x1 = ROUND_DOWN(MAX(area->x1, 0), UI_FLUSH_ALIGN_X);
	area->y1 = ROUND_DOWN(MAX(area->y1, 0), UI_FLUSH_ALIGN_Y);
	area->x2 = MIN(ROUND_UP(area->x2 + 1, UI_FLUSH_ALIGN_X), UI_SCREEN_W) - 1;
	area->y2 = MIN(ROUND_UP(area->y2 + 1, UI_FLUSH_ALIGN_Y), UI_SCREEN_H) - 1;
}
#endif

static int32_t sweep_rpm(int64_t elapsed_ms, int32_t rpm_max)
{
	const int32_t phase = (int32_t)(elapsed_ms % SWEEP_PERIOD_MS);

	return (int32_t)(((int64_t)phase * rpm_max) / SWEEP_PERIOD_MS);
}

static void log_heartbeat(int32_t rpm, const struct frame_stats *stats, int64_t window_ms)
{
#ifdef CONFIG_JUMPBACK_FPS_COUNTER
	const uint32_t fps_tenths =
		(uint32_t)(stats->frames * TENTHS_PER_UNIT * MSEC_PER_SEC / window_ms);
	const uint32_t render_ms =
		stats->frames ? (uint32_t)(stats->render_ms / stats->frames) : 0;
	char frame_rate[48];

	snprintk(frame_rate, sizeof(frame_rate), " | %u.%u fps | render %u ms",
		 fps_tenths / TENTHS_PER_UNIT, fps_tenths % TENTHS_PER_UNIT, render_ms);
#else
	ARG_UNUSED(stats);
	ARG_UNUSED(window_ms);
	const char *frame_rate = "";
#endif

	LOG_INF("alive: rpm=%d%s", rpm, frame_rate);
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

#if DISPLAY_IS_ST7306_LANDSCAPE
	lv_display_add_event_cb(lv_display_get_default(), round_area_to_panel_units,
				LV_EVENT_INVALIDATE_AREA, NULL);
#endif

	screen = lv_screen_active();
	lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), LV_PART_MAIN);
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
	struct frame_stats stats = {0};

	while (true) {
		const int32_t rpm = sweep_rpm(k_uptime_get() - start, rpm_max);

		tacho_set_rpm(&tacho, rpm);

		const int64_t render_start = k_uptime_get();

		lv_timer_handler();

		stats.render_ms += k_uptime_get() - render_start;
		stats.frames++;

		const int64_t since_heartbeat = k_uptime_get() - last_heartbeat;

		if (since_heartbeat >= HEARTBEAT_INTERVAL_MS) {
			last_heartbeat = k_uptime_get();
			log_heartbeat(rpm, &stats, since_heartbeat);
			stats = (struct frame_stats){0};
		}

		sleep_until_next_frame(&next_frame);
	}

	return 0;
}
