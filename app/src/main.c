/*
 * Copyright 2026 Michał Bogdziewicz michal@bogdziewicz.xyz
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

int main(void)
{
	LOG_INF("CWbeat %s", APP_VERSION_STRING);
	return 0;
}
