#ifndef TACHO_H_
#define TACHO_H_

#include <lvgl.h>
#include <stdint.h>

struct tacho_config {
	// Canvas size in pixels. Zero means 300x220.
	int32_t width;
	int32_t height;

	// Displayed range. rpm_max must exceed rpm_min.
	int32_t rpm_min;
	int32_t rpm_max;

	// RPM between labelled ticks. Zero means 1000.
	int32_t tick_interval;

	// Track and fill colours as 0xRRGGBB. Zero means the built-in theme.
	uint32_t color_track;
	uint32_t color_fill;
};

struct tacho {
	lv_obj_t *canvas;
	struct tacho_config cfg;
	int32_t rpm;

	// Pixel buffer backing the canvas, supplied by the caller.
	void *buf;
};

/*
 * Canvas pixel format, matched to the depth LVGL was built for. The reflective
 * LCD boards pin LV_COLOR_DEPTH to 1, where the only indexed format the
 * software blender can render into is I1; elsewhere the gauge stays RGB565.
 *
 * Kept together with TACHO_BUF_SIZE because the two have to agree: sizing a
 * buffer for one format and handing it to a canvas configured for the other
 * would overrun it.
 */
#if LV_COLOR_DEPTH == 1
#define TACHO_COLOR_FORMAT LV_COLOR_FORMAT_I1
#define TACHO_BPP          1
#else
#define TACHO_COLOR_FORMAT LV_COLOR_FORMAT_RGB565
#define TACHO_BPP          16
#endif

/**
 * @brief Bytes of pixel buffer a gauge of the given size needs.
 *
 * Usable in a static array size, so callers can size a buffer at compile time.
 */
#define TACHO_BUF_SIZE(w, h) LV_CANVAS_BUF_SIZE(w, h, TACHO_BPP, LV_DRAW_BUF_STRIDE_ALIGN)

int tacho_init(struct tacho *t,
	lv_obj_t *parent,
	const struct tacho_config *cfg,
	void *buf, size_t buf_size);

void tacho_set_rpm(struct tacho *t, int32_t rpm);

int32_t tacho_get_rpm(const struct tacho *t);

int32_t tacho_get_rpm_max(const struct tacho *t);

#endif /* TACHO_H_ */
