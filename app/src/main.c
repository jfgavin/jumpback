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
 * How long the needle takes to cross the full range, in milliseconds. One
 * complete up-and-back cycle is twice this.
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
 * Round an invalidated area out to what the controller can address.
 *
 * Registered after the Zephyr glue's own rounder, so it runs second and has
 * the final say. The glue's mono rounder aligns with power-of-two bitmasks,
 * which cannot express this panel's 12-pixel column unit; without this the
 * driver would reject the flush.
 *
 * Only correct while the display is the ST7306 in landscape. On native_sim
 * the SDL panel has no such constraint, so the whole callback is compiled out
 * and LVGL's own areas are used unchanged.
 */
#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_display), waveshare_st7306_landscape)
static void round_area_cb(lv_event_t *e)
{
	lv_area_t *area = lv_event_get_param(e);

	lv_area_t span;

	/*
	 * Narrow a canvas-wide invalidation to what the gauge actually
	 * changed. lv_canvas_finish_layer() marks the whole canvas dirty at
	 * the end of every redraw, and since the canvas covers the screen
	 * that would flush all 120000 pixels however little moved.
	 */
	if (tacho_dirty_span(&span) && lv_area_get_width(area) >= UI_SCREEN_W &&
	    lv_area_get_height(area) >= UI_SCREEN_H) {
		*area = span;
	}

	/* Defensive: LVGL may hand over areas reaching past the screen. */
	if (area->x1 < 0) {
		area->x1 = 0;
	}
	if (area->y1 < 0) {
		area->y1 = 0;
	}

	area->x1 = (area->x1 / UI_FLUSH_ALIGN_X) * UI_FLUSH_ALIGN_X;
	area->x2 = ((area->x2 / UI_FLUSH_ALIGN_X) + 1) * UI_FLUSH_ALIGN_X - 1;

	area->y1 = (area->y1 / UI_FLUSH_ALIGN_Y) * UI_FLUSH_ALIGN_Y;
	area->y2 = ((area->y2 / UI_FLUSH_ALIGN_Y) + 1) * UI_FLUSH_ALIGN_Y - 1;

	/* Rounding out can overshoot the screen; the driver rejects that. */
	if (area->x2 >= UI_SCREEN_W) {
		area->x2 = UI_SCREEN_W - 1;
	}
	if (area->y2 >= UI_SCREEN_H) {
		area->y2 = UI_SCREEN_H - 1;
	}
}
#endif

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

#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_display), waveshare_st7306_landscape)
	/*
	 * Added after lvgl_display.c has registered the glue's rounder, so
	 * this one runs last and its alignment is what reaches the driver.
	 */
	lv_display_add_event_cb(lv_display_get_default(), round_area_cb,
				LV_EVENT_INVALIDATE_AREA, NULL);
#endif

	screen = lv_screen_active();

	// Set static background
	lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BG), LV_PART_MAIN);

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
#ifdef CONFIG_JUMPBACK_FPS_COUNTER
	/* Frames and cumulative render time since the last heartbeat. */
	uint32_t frames = 0;
	int64_t render_total = 0;
#endif

	while (1) {
		/*
		 * Needle position from elapsed time, so the sweep takes
		 * SWEEP_PERIOD_MS regardless of how fast this loop turns. A
		 * platform that renders slowly shows fewer intermediate
		 * positions rather than sweeping more slowly.
		 */
		int64_t elapsed = k_uptime_get() - start;
		int32_t phase = (int32_t)(elapsed % (2 * SWEEP_PERIOD_MS));
		int32_t rpm;

		/*
		 * Sweep up and then back down, rather than snapping from full
		 * scale to zero.
		 *
		 * A sawtooth costs a full-screen flush once per cycle: the
		 * needle jumps the whole range in one frame, so the span
		 * between the old and new fill ends is the entire track and
		 * the dirty rectangle covers the panel. Reversing keeps every
		 * step small, so every frame stays a narrow band - and it
		 * reads better on the glass than a snap back.
		 */
		if (phase >= SWEEP_PERIOD_MS) {
			phase = 2 * SWEEP_PERIOD_MS - phase;
		}

		rpm = (int32_t)(((int64_t)phase * rpm_max) / SWEEP_PERIOD_MS);

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
#ifdef CONFIG_JUMPBACK_FPS_COUNTER
		const int64_t render_start = k_uptime_get();
#endif

		lv_timer_handler();

#ifdef CONFIG_JUMPBACK_FPS_COUNTER
		render_total += k_uptime_get() - render_start;
		frames++;
#endif

		/*
		 * Reported on the same timer as the heartbeat so the counter
		 * adds no console traffic of its own - logging is expensive
		 * enough here to skew what it is measuring.
		 */
		if (k_uptime_get() - last_log >= 1000) {
#ifdef CONFIG_JUMPBACK_FPS_COUNTER
			const int64_t window = k_uptime_get() - last_log;
#endif

			last_log = k_uptime_get();
#ifdef CONFIG_JUMPBACK_FPS_COUNTER
			/*
			 * Scaled by 10 rather than floated: a tenth of a frame
			 * per second is finer than the measurement is stable
			 * to, and this build has no float printf.
			 */
			LOG_INF("alive: rpm=%d | %u.%u fps | render %u ms", rpm,
				(unsigned int)(frames * 1000 / window),
				(unsigned int)((frames * 10000 / window) % 10),
				(unsigned int)(frames ? render_total / frames : 0));
			frames = 0;
			render_total = 0;
#else
			LOG_INF("alive: rpm=%d", rpm);
#endif
		}

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
