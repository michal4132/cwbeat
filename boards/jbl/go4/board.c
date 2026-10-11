/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/init.h>
#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/peer/ws9652a.h>
#include <zephyr/soc/ws9652a/counter.h>
#include <cwbeat/boards/jbl_go4/power_off.h>
#include <zephyr/drivers/clock_control/ws9652a.h>

#define GO4_BOARD_INIT_PRIORITY 45
#define BOOT_SAMPLE_GAP_US 10u
#define BOOT_SAMPLE_POLL_LIMIT 4096u

static bool boot_requested;
static const struct gpio_dt_spec power_button =
	GPIO_DT_SPEC_GET(DT_NODELABEL(power_button), gpios);
static const struct device *const clock_controller = DEVICE_DT_GET(DT_NODELABEL(clkctrl));
static const struct gpio_dt_spec power_hold =
	GPIO_DT_SPEC_GET(DT_NODELABEL(power_controls), hold_gpios);

BUILD_ASSERT(CONFIG_CLOCK_CONTROL_INIT_PRIORITY < GO4_BOARD_INIT_PRIORITY,
	     "Go 4 boot sampling requires an initialized clock controller");
BUILD_ASSERT(CONFIG_GPIO_INIT_PRIORITY < GO4_BOARD_INIT_PRIORITY,
	     "Go 4 boot sampling requires initialized GPIO devices");
BUILD_ASSERT(DT_SAME_NODE(DT_GPIO_CTLR(DT_NODELABEL(power_controls), hold_gpios),
			 DT_GPIO_CTLR(DT_NODELABEL(power_button), gpios)) &&
	     DT_GPIO_PIN(DT_NODELABEL(power_controls), hold_gpios) ==
		     DT_GPIO_PIN(DT_NODELABEL(power_button), gpios) &&
	     DT_GPIO_FLAGS(DT_NODELABEL(power_controls), hold_gpios) == GPIO_ACTIVE_HIGH &&
	     DT_GPIO_FLAGS(DT_NODELABEL(power_button), gpios) ==
		     (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN),
	     "Power input needs a pull-down and shares its active-high GPIO with the hold latch");

bool power_boot_requested(void)
{
	return boot_requested;
}

static bool power_pressed_at_boot(void)
{
	/* TOP 0x4D398: configure GPIO0.0 as an active-high input with its 22 kOhm
	 * pull-down, preserving its separate power-hold latch, then sample it three times.
	 * This matches the canonical Go 4 policy (Power required), not every
	 * ATS startup-policy variant supported by the original TOP. */
	if (gpio_pin_configure_dt(&power_button, GPIO_INPUT)) {
		return false;
	}
	uint32_t hz;
	if (clock_control_get_rate(clock_controller, (clock_control_subsys_t)WS9652A_CLOCK_CORE,
				   &hz) || !ws9652a_counter_enable()) {
		return false;
	}
	uint32_t cycles = DIV_ROUND_UP(hz, 1000000u / BOOT_SAMPLE_GAP_US);
	bool pressed = true;
	for (unsigned sample = 0; sample < 3; ++sample) {
		pressed &= gpio_pin_get_dt(&power_button) > 0;
		if (sample < 2 && !ws9652a_counter_wait(cycles, BOOT_SAMPLE_POLL_LIMIT)) {
			return false;
		}
	}
	return pressed;
}

static int go4_board_init(void)
{
	if (!device_is_ready(clock_controller) || !gpio_is_ready_dt(&power_button) ||
	    !gpio_is_ready_dt(&power_hold)) {
		return -ENODEV;
	}
	/* GPIO0.0 has a separate output latch: configuring it as an output
	 * preserves the button input and the inherited latch level. */
	int result = gpio_pin_configure_dt(&power_hold, GPIO_OUTPUT);
	if (result) {
		return result;
	}
	boot_requested = power_pressed_at_boot();
	if (!boot_requested) {
		/* A warm reset can leave independently running peers behind. Stop
		 * them before releasing the latch, even if USB keeps TOP powered.
		 * A missing DSP acknowledgment must not permit automatic startup. */
		(void)ws9652a_peer_hold_emergency();
		ws9652a_peer_controller_stop(DEVICE_DT_GET(DT_NODELABEL(peer_ctrl)));
	}
	return gpio_pin_set_dt(&power_hold, boot_requested);
}
SYS_INIT(go4_board_init, PRE_KERNEL_1, GO4_BOARD_INIT_PRIORITY);
