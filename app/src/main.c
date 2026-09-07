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
 * is the floor that the glass can actually show; the RPM step below is what
 * governs how fast the needle sweeps.
 */
#define FRAME_INTERVAL_MS 20

/* RPM added per frame. Larger steps make the sweep visibly faster. */
#define RPM_STEP 300

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

	int rpm = 0;
	uint32_t frame = 0;
	while (1) {
		tacho_set_rpm(&tacho, rpm);

		rpm += RPM_STEP;
		if (rpm > tacho_get_rpm_max(&tacho)) rpm = 0;

		/*
		 * Heartbeat. The USB Serial/JTAG console drops anything written
		 * while no host has the port open - poll_out waits ~50ms for
		 * FIFO space and then discards the byte - so the boot banner is
		 * long gone by the time a terminal attaches. Logging on a timer
		 * gives something that arrives after the terminal is up, which
		 * is what makes "is it running?" answerable at all here.
		 */
		if (++frame % (1000 / FRAME_INTERVAL_MS) == 0) {
			LOG_INF("alive: rpm=%d", rpm);
		}

		lv_timer_handler();
		k_msleep(FRAME_INTERVAL_MS);
	}

	return 0;
}
