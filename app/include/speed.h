/*
 * Copyright (c) 2026 Josh Gav
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPEED_H_
#define SPEED_H_

#include <stdbool.h>
#include <stdint.h>

struct speed_reading {
	uint32_t mph_tenths;
	bool valid;
	int64_t timestamp_ms;
	uint32_t msg_count;
	uint16_t satellites;
};

int speed_init(void);

void speed_get(struct speed_reading *out);

uint32_t speed_mms_to_mph_tenths(uint32_t mm_per_s);

#endif /* SPEED_H_ */
