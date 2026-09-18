#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <lvgl.h>

#include "tacho.h"
#include "ui.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/*
 * Frame period. The panel's high-power waveform tops out near 51Hz, so ~20ms
 * is the floor that the glass can actually show.
 *
 * This is a target period, not a delay: the loop below sleeps until the next
 * multiple of it rather than sleeping for it. Sleeping for it would add the
 * render time to every frame, which is what made the sweep run at different
 * speeds on different platforms - the host renders a frame in a few
 * milliseconds while the board spends far longer packing and clocking 15000
 * bytes out over SPI, so the same k_msleep() produced 30ms frames in the
 * simulator and much slower ones on the glass.
 */
#define FRAME_INTERVAL_MS 20

/*
 * How long the needle takes to sweep the full range, in milliseconds.
 *
 * The sweep is defined in time rather than in RPM-per-frame so it runs at the
 * same speed everywhere. Deriving the step from the frame rate instead ties
 * the animation to whatever rate the platform happens to achieve: at a fixed
 * 300 RPM per frame the gauge crossed its whole range in 26.7 frames, which is
 * a brisk 0.8s in the simulator and a much slower crawl on hardware, purely
 * because the two draw at different rates.
 *
 * Positions are computed from the elapsed time below, so a platform that
 * cannot keep up drops frames and still sweeps in this many milliseconds -
 * it just does so less smoothly.
 */
#define SWEEP_PERIOD_MS 3000

/*
 * Model name, drawn straight onto the screen rather than into a widget. Uses
 * an event callback because a screen has no canvas layer of its own - LVGL
 * hands one over during the draw pass.
 */
static void draw_jumpback(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_draw_label_dsc_t dsc;
	lv_area_t area;

	lv_draw_label_dsc_init(&dsc);
	dsc.font = &lv_font_montserrat_28;
	dsc.color = lv_color_hex(UI_COLOR_DIM);
	dsc.text = "JMP-1";

	area.x1 = UI_MARGIN;
	area.y1 = UI_MARGIN;
	area.x2 = UI_SCREEN_W - UI_MARGIN;
	area.y2 = UI_SCREEN_H - UI_MARGIN;

	lv_draw_label(layer, &dsc, &area);
}

int main(void)
{
	const struct device *display_dev;
	lv_obj_t *screen;
	static struct tacho tacho;
	static uint8_t tacho_buf[TACHO_BUF_SIZE(UI_GAUGE_W, UI_GAUGE_H)];
	int err;

	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device not ready");
		return 0;
	}

	screen = lv_screen_active();

	// Set static background
	lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), LV_PART_MAIN);

	// Runs on every redraw of the screen, after its own background.
	lv_obj_add_event_cb(screen, draw_jumpback, LV_EVENT_DRAW_MAIN_END, NULL);

	// Drop interactive scrollbars
	lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

	// Nothing drawn until time handler runs
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
	int64_t last_log = start;

	while (1) {
		/*
		 * Needle position from elapsed time, so the sweep takes
		 * SWEEP_PERIOD_MS regardless of how fast this loop turns. A
		 * platform that renders slowly shows fewer intermediate
		 * positions rather than sweeping more slowly.
		 */
		int64_t elapsed = k_uptime_get() - start;
		int32_t phase = (int32_t)(elapsed % SWEEP_PERIOD_MS);
		int32_t rpm = (int32_t)(((int64_t)phase * rpm_max) / SWEEP_PERIOD_MS);

		tacho_set_rpm(&tacho, rpm);

		/*
		 * Heartbeat. The USB Serial/JTAG console drops anything written
		 * while no host has the port open - poll_out waits ~50ms for
		 * FIFO space and then discards the byte - so the boot banner is
		 * long gone by the time a terminal attaches. Logging on a timer
		 * gives something that arrives after the terminal is up, which
		 * is what makes "is it running?" answerable at all here.
		 *
		 * Timed off the clock rather than counted in frames, for the
		 * same reason the needle is: a frame count logs at a different
		 * real-world rate on each platform.
		 */
		if (k_uptime_get() - last_log >= 1000) {
			last_log = k_uptime_get();
			LOG_INF("alive: rpm=%d", rpm);
		}

		lv_timer_handler();

		/*
		 * Sleep to the next frame boundary rather than for a fixed
		 * interval, so the render time is absorbed by the wait instead
		 * of being added to it. If a frame overran its slot, skip the
		 * boundaries already missed so the loop rejoins the cadence
		 * instead of accumulating lag.
		 */
		next_frame += FRAME_INTERVAL_MS;

		int64_t now = k_uptime_get();
		if (next_frame <= now) {
			next_frame = now + FRAME_INTERVAL_MS;
		} else {
			k_msleep((int32_t)(next_frame - now));
		}
	}

	return 0;
}
