/* SPDX-License-Identifier: Apache-2.0 */
/* ZGMICRO WS9652A TOP processor initialization.
 * Retained BT clock changes are reported to Zephyr for timer calibration.
 * No PLL/source/divider writes or BT/DSP release is performed. */
#include <soc.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/drivers/clock_control/ws9652a.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/util.h>

uint32_t SystemCoreClock =
	DT_PROP(DT_CLOCKS_CTLR_BY_NAME(DT_NODELABEL(clkctrl), xtal), clock_frequency);

static const struct device *const clock_controller = DEVICE_DT_GET(DT_NODELABEL(clkctrl));

static bool ws9652a_clock_sync(void)
{
	uint32_t hz;

	if (clock_control_get_rate(clock_controller, (clock_control_subsys_t)WS9652A_CLOCK_CORE,
				   &hz)) {
		return false;
	}
	if (hz != sys_clock_hw_cycles_per_sec()) {
		/* The SysTick override rescales its counters and reload. A plain
		 * assignment of the global rate would lose that state. */
		z_sys_clock_hw_cycles_per_sec_update(hz);
	}
	SystemCoreClock = hz;
	return true;
}

__weak void ws9652a_recovery_startup(void)
{
}

void soc_reset_hook(void)
{
	/* SBL hands off with interrupts potentially enabled. Check the board's
	 * recovery request before disturbing SBL's vectors or private RAM. */
	__disable_irq();
	__set_BASEPRI(0u);
	ws9652a_recovery_startup();

	/* Only inherited interrupt state is cleared. Zephyr subsequently owns
	 * SysTick and NVIC; this must never run during normal thread execution. */
	SysTick->CTRL = 0u;
	NVIC->ICER[0] = UINT32_MAX;
	NVIC->ICPR[0] = UINT32_MAX;
	SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
	__DSB();
	__ISB();
	/* INIT_ARCH_HW_AT_BOOT follows immediately in Zephyr reset.S and
	 * clears FAULTMASK, resets the remaining NVIC state, then restores
	 * PRIMASK. Keep interrupts masked until that architecture handoff. */
}

void soc_early_init_hook(void)
{
	extern unsigned int z_clock_hw_cycles_per_sec;
	uint32_t hz;

	/* Reserved clock selections are not a supported boot contract. Do not
	 * run timers at an invented rate. Recovery startup precedes this check. */
	if (ws9652a_clock_control_get_rate_early(WS9652A_CLOCK_CORE, &hz)) {
		k_panic();
	}
	SystemCoreClock = hz;
	z_clock_hw_cycles_per_sec = hz;
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* Independent of BT/UI admission. Timing remains an on-hardware acceptance
 * measurement; the nominal interval alone does not prove queue latency. */
static struct k_work_delayable clock_monitor;
static struct k_spinlock clock_stats_lock;
static struct ws9652a_clock_monitor_stats clock_stats;
static int64_t clock_previous_us;
static void clock_monitor_work(struct k_work *work)
{
	ARG_UNUSED(work);
	int64_t now = k_ticks_to_us_floor64(k_uptime_ticks());
	bool valid = ws9652a_clock_sync();
	uint32_t hz = valid ? sys_clock_hw_cycles_per_sec() : 0;
	k_spinlock_key_t key = k_spin_lock(&clock_stats_lock);
	if (clock_stats.sample_count) {
		uint32_t interval = (uint32_t)MIN(now - clock_previous_us, UINT32_MAX);
		clock_stats.max_interval_us = MAX(clock_stats.max_interval_us, interval);
	}
	clock_previous_us = now;
	if (clock_stats.sample_count != UINT32_MAX) {
		++clock_stats.sample_count;
	}
	if (valid) {
		if (!clock_stats.first_hz) {
			clock_stats.first_hz = hz;
			clock_stats.last_hz = hz;
		} else if (clock_stats.last_hz != hz) {
			if (clock_stats.frequency_changes != UINT32_MAX) {
				++clock_stats.frequency_changes;
			}
			clock_stats.last_hz = hz;
		}
	}
	if (!valid && clock_stats.failed_samples != UINT32_MAX) {
		++clock_stats.failed_samples;
	}
	k_spin_unlock(&clock_stats_lock, key);
	k_work_reschedule(&clock_monitor, K_USEC(100));
}
struct ws9652a_clock_monitor_stats ws9652a_clock_monitor_snapshot(void)
{
	k_spinlock_key_t key = k_spin_lock(&clock_stats_lock);
	struct ws9652a_clock_monitor_stats copy = clock_stats;
	k_spin_unlock(&clock_stats_lock, key);
	return copy;
}
static int clock_monitor_init(void)
{
	if (!device_is_ready(clock_controller)) {
		return -ENODEV;
	}
	k_work_init_delayable(&clock_monitor, clock_monitor_work);
	k_work_reschedule(&clock_monitor, K_NO_WAIT);
	return 0;
}
SYS_INIT(clock_monitor_init, APPLICATION, 90);
