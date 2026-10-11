/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZGMICRO_SOC_WS9652A_H_
#define ZGMICRO_SOC_WS9652A_H_

#include <cmsis_core_m_defaults.h>

#ifndef _ASMLANGUAGE
#include <stdbool.h>
#include <stdint.h>

/* Measured system-workqueue observation timing; nominal interval is 100us. */
struct ws9652a_clock_monitor_stats {
	uint32_t sample_count, max_interval_us, failed_samples;
	/* Valid observed calibrated rates; failed samples do not establish a baseline. */
	uint32_t frequency_changes, first_hz, last_hz;
};
struct ws9652a_clock_monitor_stats ws9652a_clock_monitor_snapshot(void);

/* Called before Zephyr touches data/BSS. Overrides may only use constants,
 * stack locals and hardware registers. */
void ws9652a_recovery_startup(void);
#endif

#endif
