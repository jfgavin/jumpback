/*
 * Copyright (c) 2026 Josh Gav
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include "speed.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(speed, LOG_LEVEL_INF);

#define GNSS_NODE DT_ALIAS(gnss)

#define UM_PER_MM                1000U
#define UM_PER_S_PER_MPH_TENTH   44704U

uint32_t speed_mms_to_mph_tenths(uint32_t mm_per_s)
{
	return (uint32_t)(((uint64_t)mm_per_s * UM_PER_MM) / UM_PER_S_PER_MPH_TENTH);
}

#if DT_NODE_HAS_STATUS(GNSS_NODE, okay)

#include <zephyr/drivers/gnss.h>

static struct speed_reading published;
static struct k_spinlock published_lock;
static uint32_t msg_count;

static void speed_data_cb(const struct device *dev, const struct gnss_data *data)
{
	ARG_UNUSED(dev);

	msg_count++;

	struct speed_reading reading = {
		.timestamp_ms = k_uptime_get(),
		.msg_count = msg_count,
		.satellites = data->info.satellites_cnt,
	};

	if (data->info.fix_status != GNSS_FIX_STATUS_NO_FIX) {
		reading.valid = true;
		reading.mph_tenths = speed_mms_to_mph_tenths(data->nav_data.speed);
	}

	K_SPINLOCK(&published_lock) {
		published = reading;
	}
}

GNSS_DT_DATA_CALLBACK_DEFINE(GNSS_NODE, speed_data_cb);

int speed_init(void)
{
	const struct device *dev = DEVICE_DT_GET(GNSS_NODE);

	if (!device_is_ready(dev)) {
		LOG_ERR("GNSS device not ready");
		return -ENODEV;
	}

	LOG_INF("GNSS ready, waiting for fix");
	return 0;
}

void speed_get(struct speed_reading *out)
{
	K_SPINLOCK(&published_lock) {
		*out = published;
	}
}

#else

int speed_init(void)
{
	LOG_WRN("No GNSS device in devicetree; speed readout inactive");
	return -ENODEV;
}

void speed_get(struct speed_reading *out)
{
	*out = (struct speed_reading){0};
}

#endif
