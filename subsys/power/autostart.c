/*
 * Copyright 2026 Michał Bogdziewicz michal@bogdziewicz.xyz
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cwbeat/power.h>
#include "private.h"
#include <zephyr/init.h>

static int power_autostart(void)
{
	if (power_autostart_allowed()) {
		(void)power_request(POWER_ON, NULL);
	}
	return 0;
}

SYS_INIT(power_autostart, APPLICATION, 95);
