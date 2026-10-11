/* Stock TOP 0x4D4CC -> 0x4A154(1): platform shutdown followed by reset.
 * Application STOP owns the preceding BT save/disconnect handshake.
 * Settings persistence and startup policy belong to the replacement TOP;
 * no original TOP callback or resident SBL address is callable here.
 */
#include <cwbeat/boards/jbl_go4/power_off.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/soc/ws9652a/counter.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/peer/ws9652a.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/clock_control/ws9652a.h>

#include <zephyr/soc/ws9652a/reset.h>
#define MICROSECONDS_PER_SECOND  UINT32_C(1000000)
#define DELAY_QUANTUM_US         UINT32_C(2000)
#define PEER_SETTLE_QUANTA       800u
#define RESET_SETTLE_US          UINT32_C(1000)
/* Guard a core counter that becomes unavailable after the preflight. The
 * calibrated 192MHz profile needs far fewer foreground polls per quantum. */
#define DELAY_QUANTUM_POLL_LIMIT UINT32_C(131072)
static const struct device *const clock_controller = DEVICE_DT_GET(DT_NODELABEL(clkctrl));
static const struct gpio_dt_spec power_hold =
	GPIO_DT_SPEC_GET(DT_NODELABEL(power_controls), hold_gpios);
static const struct gpio_dt_spec latch_clear =
	GPIO_DT_SPEC_GET(DT_NODELABEL(power_controls), latch_clear_gpios);

static void delay_us(uint32_t duration, uint32_t cycles_per_us)
{
	(void)ws9652a_counter_wait(duration * cycles_per_us, DELAY_QUANTUM_POLL_LIMIT);
}

bool power_off_request(void)
{
	if (!device_is_ready(clock_controller) || !gpio_is_ready_dt(&power_hold) ||
	    !gpio_is_ready_dt(&latch_clear)) {
		return false;
	}
	if (!ws9652a_counter_enable()) {
		return false;
	}
	uint32_t hz;
	if (clock_control_get_rate(clock_controller, (clock_control_subsys_t)WS9652A_CLOCK_CORE,
				   &hz)) {
		return false;
	}
	uint32_t cycles_per_us = (hz + MICROSECONDS_PER_SECOND - 1u) / MICROSECONDS_PER_SECOND;
	__asm__ volatile("cpsid i" ::: "memory");
	/* Drain/stop is complete at the caller; disconnect our replacement USB
	 * console before following the original platform register sequence. */
	ws9652a_usb_detach_emergency();
	(void)gpio_pin_set_dt(&power_hold, 0);
	(void)wdt_disable(DEVICE_DT_GET(DT_NODELABEL(wdt0)));
	delay_us(DELAY_QUANTUM_US, cycles_per_us);
	ws9652a_peer_bt_stop(DEVICE_DT_GET(DT_NODELABEL(peer_ctrl)));
	ws9652a_peer_controller_stop(DEVICE_DT_GET(DT_NODELABEL(peer_ctrl)));
	/* Stock clears this latch without changing the Play button's input mode. */
	(void)gpio_pin_set_raw(latch_clear.port, latch_clear.pin, 0);
	(void)ws9652a_peer_dsp_hold_emergency();
	for (unsigned remaining = PEER_SETTLE_QUANTA; remaining != 0; --remaining) {
		delay_us(DELAY_QUANTUM_US, cycles_per_us);
	}
	delay_us(RESET_SETTLE_US, cycles_per_us);
	ws9652a_reset_immediate();
}
