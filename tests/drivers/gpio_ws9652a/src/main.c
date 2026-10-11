/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl/ws9652a.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define PORT_BASE  0x1000u
#define ALIAS_BASE 0x2000u
static uint32_t registers[16];
static struct {
	uintptr_t address;
	uint32_t value;
} writes[32];
static unsigned int write_count, port_reads, lock_calls;
static bool pad_locked;
struct k_spinlock ws9652a_pad_lock;

/* Trace the shared lock boundary as well as the GPIO data accesses. The
 * production provider tests exercise pad MMIO and power-latch preservation. */
static k_spinlock_key_t mock_lock(struct k_spinlock *lock)
{
	zassert_equal_ptr(lock, &ws9652a_pad_lock);
	zassert_false(pad_locked);
	k_spinlock_key_t key = k_spin_lock(lock);
	pad_locked = true;
	++lock_calls;
	return key;
}

static void mock_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
	zassert_equal_ptr(lock, &ws9652a_pad_lock);
	zassert_true(pad_locked);
	pad_locked = false;
	k_spin_unlock(lock, key);
}

static uint32_t mock_read(uintptr_t address)
{
	zassert_true(pad_locked);
	zassert_true(address >= PORT_BASE && address < PORT_BASE + sizeof(registers));
	if (address == PORT_BASE) {
		port_reads++;
	}
	return registers[(address - PORT_BASE) / sizeof(uint32_t)];
}

static void mock_write(uint32_t value, uintptr_t address)
{
	zassert_true(pad_locked);
	zassert_true(write_count < ARRAY_SIZE(writes));
	writes[write_count].address = address;
	writes[write_count++].value = value;
	/* GPIO runtime writes must use per-pin aliases, never whole-port RMW. */
	zassert_true(address >= ALIAS_BASE && address < ALIAS_BASE + 32u * sizeof(uint32_t));
	uint32_t mask = BIT((address - ALIAS_BASE) / sizeof(uint32_t));
	registers[0] = (registers[0] & ~mask) | (value ? mask : 0u);
}

static int pinctrl_result;
static unsigned int pinctrl_calls, last_bank;
static uint32_t last_mask, last_configuration;
static int mock_pinctrl_configure(unsigned bank, uint32_t mask, uint32_t configuration)
{
	/* The provider takes the shared lock internally. Taking it in the GPIO
	 * caller as well would deadlock on a multicore build. */
	zassert_false(pad_locked);
	pinctrl_calls++;
	last_bank = bank;
	last_mask = mask;
	last_configuration = configuration;
	return pinctrl_result;
}

#define ws9652a_pinctrl_gpio_configure mock_pinctrl_configure
#define k_spin_lock                    mock_lock
#define k_spin_unlock                  mock_unlock
#define sys_read32                     mock_read
#define sys_write32                    mock_write
#include "../../../../drivers/gpio/zgmicro/gpio_ws9652a.c"

static const struct device *const test_device = DEVICE_DT_GET(DT_NODELABEL(test_gpio));
static const struct gpio_dt_spec reconfig = GPIO_DT_SPEC_GET(DT_NODELABEL(reconfig_pin), gpios);
static const struct gpio_dt_spec active_low = GPIO_DT_SPEC_GET(DT_NODELABEL(active_low_pin), gpios);
static const struct gpio_dt_spec power_hold = GPIO_DT_SPEC_GET(DT_NODELABEL(power_hold_pin), gpios);
static const struct gpio_dt_spec pull_down = GPIO_DT_SPEC_GET(DT_NODELABEL(pull_down_pin), gpios);

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	memset(registers, 0xff, sizeof(registers));
	memset(test_device->data, 0, sizeof(struct ws9652a_gpio_data));
	write_count = 0;
	port_reads = 0;
	lock_calls = 0;
	pad_locked = false;
	pinctrl_result = 0;
	pinctrl_calls = 0;
}

ZTEST(ws9652a_gpio, test_output_configuration_delegates_without_latch_override)
{
	zassert_ok(gpio_pin_configure(test_device, 16, GPIO_OUTPUT));
	zassert_equal(pinctrl_calls, 1);
	zassert_equal(last_bank, 0);
	zassert_equal(last_mask, BIT(16));
	zassert_equal(last_configuration, WS9652A_PINCFG_OUTPUT);
	zassert_equal(write_count, 0);
	zassert_equal(port_reads, 0);
	zassert_equal(lock_calls, 0);
}

ZTEST(ws9652a_gpio, test_output_initial_levels_and_power_pin_reach_provider)
{
	zassert_ok(gpio_pin_configure(test_device, 16, GPIO_OUTPUT_LOW));
	zassert_equal(last_mask, BIT(16));
	zassert_equal(last_configuration, WS9652A_PINCFG_OUTPUT | WS9652A_PINCFG_OUTPUT_LOW);
	zassert_ok(gpio_pin_configure(test_device, 12, GPIO_OUTPUT_HIGH));
	zassert_equal(last_mask, BIT(12));
	zassert_equal(last_configuration, WS9652A_PINCFG_OUTPUT | WS9652A_PINCFG_OUTPUT_HIGH);
	zassert_ok(gpio_pin_configure(test_device, 0, GPIO_OUTPUT_LOW));
	zassert_equal(last_mask, BIT(0));
	zassert_equal(last_configuration, WS9652A_PINCFG_OUTPUT | WS9652A_PINCFG_OUTPUT_LOW);
	zassert_equal(pinctrl_calls, 3);
	zassert_equal(write_count, 0);
	zassert_equal(lock_calls, 0);
}

ZTEST(ws9652a_gpio, test_input_and_power_input_use_input_profile)
{
	zassert_ok(gpio_pin_configure(test_device, 1, GPIO_INPUT | GPIO_ACTIVE_LOW));
	zassert_equal(last_mask, BIT(1));
	zassert_equal(last_configuration, WS9652A_PINCFG_INPUT);
	zassert_ok(gpio_pin_configure(test_device, 0, GPIO_INPUT));
	zassert_equal(last_mask, BIT(0));
	zassert_equal(last_configuration, WS9652A_PINCFG_INPUT);
	zassert_equal(write_count, 0);
	zassert_equal(port_reads, 0);
	zassert_equal(lock_calls, 0);
}

ZTEST(ws9652a_gpio, test_masked_write_has_no_port_read_or_write)
{
	zassert_ok(gpio_port_set_masked_raw(test_device, BIT(12) | BIT(16), BIT(16)));
	zassert_equal(port_reads, 0);
	zassert_equal(write_count, 2);
	zassert_equal(writes[0].address, ALIAS_BASE + 12u * 4u);
	zassert_equal(writes[1].address, ALIAS_BASE + 16u * 4u);
	zassert_equal(registers[0], UINT32_MAX & ~BIT(12));
	zassert_equal(registers[0] & BIT(0), BIT(0));
	zassert_equal(lock_calls, 1);
	zassert_false(pad_locked);
}

ZTEST(ws9652a_gpio, test_dt_common_config_honors_ngpios_and_reserved_ranges)
{
	const struct gpio_driver_config *common = test_device->config;
	zassert_true(device_is_ready(test_device));
	zassert_equal(common->port_pin_mask, GENMASK(23, 0) & ~(BIT(7) | BIT(8) | BIT(14)));
}

ZTEST(ws9652a_gpio, test_reserved_and_out_of_range_pins_do_not_touch_hardware)
{
	const gpio_pin_t invalid[] = {7, 8, 14, 24, 31, 32};

	/* The public single-pin wrapper asserts these caller errors. Exercise
	 * the driver's defensive rejection directly, before its BIT(pin) use. */
	for (unsigned i = 0; i < ARRAY_SIZE(invalid); ++i) {
		zassert_equal(ws9652a_gpio_configure(test_device, invalid[i], GPIO_OUTPUT),
			      -EINVAL);
		zassert_equal(ws9652a_gpio_configure(test_device, invalid[i], GPIO_INPUT), -EINVAL);
	}
	const uint32_t masks[] = {BIT(7), BIT(8), BIT(14), BIT(24), BIT(31)};
	for (unsigned i = 0; i < ARRAY_SIZE(masks); ++i) {
		zassert_equal(gpio_port_set_bits_raw(test_device, masks[i] | BIT(1)), -ENOTSUP);
		zassert_equal(gpio_port_clear_bits_raw(test_device, masks[i]), -ENOTSUP);
		zassert_equal(gpio_port_toggle_bits(test_device, masks[i]), -ENOTSUP);
	}
	zassert_equal(write_count, 0);
	zassert_equal(pinctrl_calls, 0);
	zassert_equal(lock_calls, 0);
}

ZTEST(ws9652a_gpio, test_consumers_choose_and_change_direction)
{
	zassert_ok(gpio_pin_configure_dt(&reconfig, GPIO_INPUT));
	zassert_equal(last_configuration, WS9652A_PINCFG_INPUT);
	zassert_ok(gpio_pin_configure_dt(&reconfig, GPIO_OUTPUT));
	zassert_equal(last_configuration, WS9652A_PINCFG_OUTPUT);
	zassert_ok(gpio_pin_configure_dt(&reconfig, GPIO_INPUT));
	zassert_equal(last_configuration, WS9652A_PINCFG_INPUT);
	zassert_equal(last_mask, BIT(1));
	/* Pin16 used to be output-only; it is also a valid input. */
	zassert_ok(gpio_pin_configure(test_device, 16, GPIO_INPUT));
	zassert_equal(last_mask, BIT(16));
	zassert_equal(last_configuration, WS9652A_PINCFG_INPUT);
	zassert_equal(pinctrl_calls, 4);
	zassert_equal(write_count, 0);
}

ZTEST(ws9652a_gpio, test_active_low_dt_output_initialization_uses_physical_levels)
{
	zassert_ok(gpio_pin_configure_dt(&active_low, GPIO_OUTPUT_INACTIVE));
	zassert_equal(last_mask, BIT(16));
	zassert_equal(last_configuration, WS9652A_PINCFG_OUTPUT | WS9652A_PINCFG_OUTPUT_HIGH);
	zassert_ok(gpio_pin_configure_dt(&active_low, GPIO_OUTPUT_ACTIVE));
	zassert_equal(last_configuration, WS9652A_PINCFG_OUTPUT | WS9652A_PINCFG_OUTPUT_LOW);
	/* Explicit physical levels remain independent of the active-low flag. */
	zassert_ok(gpio_pin_configure_dt(&active_low, GPIO_OUTPUT_LOW));
	zassert_equal(last_configuration, WS9652A_PINCFG_OUTPUT | WS9652A_PINCFG_OUTPUT_LOW);
	zassert_equal(pinctrl_calls, 3);
}

ZTEST(ws9652a_gpio, test_active_high_dt_button_reads_pressed_as_active)
{
	/* Volume+ is physically low when released and high when pressed. */
	zassert_ok(gpio_pin_configure_dt(&pull_down, GPIO_INPUT));
	zassert_equal(last_configuration, WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_PULL_DOWN);
	registers[GPIO_INPUT_VALUE / 4] = 0;
	zassert_equal(gpio_pin_get_dt(&pull_down), 0);
	zassert_equal(gpio_pin_get_raw(pull_down.port, pull_down.pin), 0);
	registers[GPIO_INPUT_VALUE / 4] = BIT(pull_down.pin);
	zassert_equal(gpio_pin_get_dt(&pull_down), 1);
	zassert_equal(gpio_pin_get_raw(pull_down.port, pull_down.pin), 1);
	zassert_equal(write_count, 0);
}

ZTEST(ws9652a_gpio, test_active_low_dt_input_keeps_raw_reads_physical)
{
	gpio_port_value_t value;

	zassert_ok(gpio_pin_configure_dt(&active_low, GPIO_INPUT));
	registers[GPIO_INPUT_VALUE / 4] = 0;
	zassert_equal(gpio_pin_get_dt(&active_low), 1);
	zassert_equal(gpio_pin_get_raw(active_low.port, active_low.pin), 0);
	zassert_ok(gpio_port_get_raw(test_device, &value));
	zassert_equal(value, 0);
	registers[GPIO_INPUT_VALUE / 4] = BIT(active_low.pin);
	zassert_equal(gpio_pin_get_dt(&active_low), 0);
	zassert_equal(gpio_pin_get_raw(active_low.port, active_low.pin), 1);
	zassert_ok(gpio_port_get_raw(test_device, &value));
	zassert_equal(value, BIT(active_low.pin));
	zassert_equal(write_count, 0);
}

ZTEST(ws9652a_gpio, test_mixed_dt_polarities_apply_once_per_pin)
{
	gpio_port_value_t value;

	zassert_ok(gpio_pin_configure_dt(&pull_down, GPIO_INPUT));
	zassert_ok(gpio_pin_configure_dt(&active_low, GPIO_INPUT));
	/* Both inputs active, plus one unconfigured physical high and invalid pins. */
	registers[GPIO_INPUT_VALUE / 4] = BIT(pull_down.pin) | BIT(3) | BIT(7) | BIT(24);
	zassert_ok(gpio_port_get_raw(test_device, &value));
	zassert_equal(value, BIT(pull_down.pin) | BIT(3));
	zassert_ok(gpio_port_get(test_device, &value));
	zassert_equal(value, BIT(pull_down.pin) | BIT(active_low.pin) | BIT(3));
	zassert_equal(gpio_pin_get_dt(&pull_down), 1);
	zassert_equal(gpio_pin_get_dt(&active_low), 1);
	/* Both inputs inactive: inversion must leave other physical bits alone. */
	registers[GPIO_INPUT_VALUE / 4] = BIT(active_low.pin) | BIT(3);
	zassert_ok(gpio_port_get_raw(test_device, &value));
	zassert_equal(value, BIT(active_low.pin) | BIT(3));
	zassert_ok(gpio_port_get(test_device, &value));
	zassert_equal(value, BIT(3));
	zassert_equal(gpio_pin_get_dt(&pull_down), 0);
	zassert_equal(gpio_pin_get_dt(&active_low), 0);
	zassert_equal(write_count, 0);
}

ZTEST(ws9652a_gpio, test_reconfigure_active_low_to_dt_active_high_clears_inversion)
{
	zassert_ok(gpio_pin_configure(pull_down.port, pull_down.pin,
				      GPIO_INPUT | GPIO_ACTIVE_LOW | GPIO_PULL_DOWN));
	registers[GPIO_INPUT_VALUE / 4] = 0;
	zassert_equal(gpio_pin_get_dt(&pull_down), 1);
	zassert_equal(gpio_pin_get_raw(pull_down.port, pull_down.pin), 0);
	/* Reapplying the active-high DTS specification must clear the old inversion. */
	zassert_ok(gpio_pin_configure_dt(&pull_down, GPIO_INPUT));
	zassert_equal(gpio_pin_get_dt(&pull_down), 0);
	zassert_equal(gpio_pin_get_raw(pull_down.port, pull_down.pin), 0);
	registers[GPIO_INPUT_VALUE / 4] = BIT(pull_down.pin);
	zassert_equal(gpio_pin_get_dt(&pull_down), 1);
	zassert_equal(gpio_pin_get_raw(pull_down.port, pull_down.pin), 1);
	zassert_equal(last_configuration, WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_PULL_DOWN);
	zassert_equal(write_count, 0);
}

ZTEST(ws9652a_gpio, test_data_writes_use_valid_pins_independent_of_direction)
{
	zassert_ok(gpio_pin_configure_dt(&reconfig, GPIO_INPUT));
	zassert_ok(gpio_port_clear_bits_raw(test_device, BIT(1) | BIT(23)));
	zassert_equal(registers[0], UINT32_MAX & ~(BIT(1) | BIT(23)));
	zassert_equal(writes[0].address, ALIAS_BASE + 1u * 4u);
	zassert_equal(writes[1].address, ALIAS_BASE + 23u * 4u);
	zassert_ok(gpio_port_set_bits_raw(test_device, BIT(1)));
	zassert_equal(registers[0], UINT32_MAX & ~BIT(23));
	zassert_equal(write_count, 3);
	zassert_equal(port_reads, 0);
}

ZTEST(ws9652a_gpio, test_dt_set_preserves_power_input_and_unrelated_latches)
{
	uint32_t pad_state[ARRAY_SIZE(registers) - 1];

	zassert_ok(gpio_pin_configure_dt(&power_hold, GPIO_INPUT));
	registers[GPIO_INPUT_VALUE / 4] = BIT(0) | BIT(3);
	memcpy(pad_state, &registers[1], sizeof(pad_state));

	zassert_ok(gpio_pin_set_dt(&power_hold, 0));
	zassert_equal(registers[0], UINT32_MAX & ~BIT(0));
	zassert_mem_equal(&registers[1], pad_state, sizeof(pad_state));
	zassert_equal(gpio_pin_get_dt(&power_hold), 1);
	zassert_ok(gpio_pin_set_dt(&power_hold, 1));
	zassert_equal(registers[0], UINT32_MAX);
	zassert_mem_equal(&registers[1], pad_state, sizeof(pad_state));
	zassert_equal(pinctrl_calls, 1);
	zassert_equal(port_reads, 0);
	zassert_equal(write_count, 2);
	zassert_equal(writes[0].address, ALIAS_BASE);
	zassert_equal(writes[0].value, 0);
	zassert_equal(writes[1].address, ALIAS_BASE);
	zassert_equal(writes[1].value, 1);
}

ZTEST(ws9652a_gpio, test_active_low_dt_set_uses_physical_latch_levels)
{
	zassert_ok(gpio_pin_configure_dt(&active_low, GPIO_OUTPUT));
	zassert_ok(gpio_pin_set_dt(&active_low, 1));
	zassert_equal(registers[0], UINT32_MAX & ~BIT(16));
	zassert_ok(gpio_pin_set_dt(&active_low, 0));
	zassert_equal(registers[0], UINT32_MAX);
	zassert_equal(pinctrl_calls, 1);
	zassert_equal(port_reads, 0);
	zassert_equal(write_count, 2);
	zassert_equal(writes[0].address, ALIAS_BASE + 16u * 4u);
	zassert_equal(writes[0].value, 0);
	zassert_equal(writes[1].address, ALIAS_BASE + 16u * 4u);
	zassert_equal(writes[1].value, 1);
}

ZTEST(ws9652a_gpio, test_pinctrl_consumer_propagates_failure)
{
	pinctrl_result = -EIO;
	zassert_equal(gpio_pin_configure(test_device, 16, GPIO_OUTPUT_LOW), -EIO);
	zassert_equal(gpio_pin_configure(test_device, 1, GPIO_INPUT), -EIO);
	zassert_equal(pinctrl_calls, 2);
	zassert_equal(write_count, 0);
	zassert_equal(lock_calls, 0);
}

ZTEST(ws9652a_gpio, test_bank1_output_routes_correct_bank)
{
	struct ws9652a_gpio_config bank1 = *(const struct ws9652a_gpio_config *)test_device->config;
	struct device bank1_device = {
		.config = &bank1,
		.data = test_device->data,
		.api = &ws9652a_gpio_api,
	};

	bank1.bank = 1;
	zassert_ok(gpio_pin_configure(&bank1_device, 16, GPIO_OUTPUT_LOW));
	zassert_equal(last_bank, 1);
	zassert_equal(last_mask, BIT(16));
	zassert_equal(last_configuration, WS9652A_PINCFG_OUTPUT | WS9652A_PINCFG_OUTPUT_LOW);
	zassert_ok(gpio_pin_configure(&bank1_device, 16, GPIO_INPUT | GPIO_PULL_DOWN));
	zassert_equal(last_bank, 1);
	zassert_equal(last_configuration, WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_PULL_DOWN);
	zassert_ok(gpio_pin_configure(&bank1_device, 16, GPIO_INPUT));
	zassert_equal(last_bank, 1);
	zassert_equal(last_configuration, WS9652A_PINCFG_INPUT);
	zassert_equal(write_count, 0);
}

ZTEST(ws9652a_gpio, test_pull_down_configuration_and_unbiased_transition)
{
	zassert_ok(gpio_pin_configure_dt(&pull_down, GPIO_INPUT));
	zassert_equal(last_mask, BIT(5));
	zassert_equal(last_configuration, WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_PULL_DOWN);
	zassert_ok(gpio_pin_configure(test_device, 5, GPIO_INPUT));
	zassert_equal(last_configuration, WS9652A_PINCFG_INPUT);
	zassert_ok(gpio_pin_configure(test_device, 5, GPIO_OUTPUT_HIGH | GPIO_PULL_DOWN));
	zassert_equal(last_configuration, WS9652A_PINCFG_OUTPUT | WS9652A_PINCFG_OUTPUT_HIGH |
					 WS9652A_PINCFG_BIAS_PULL_DOWN);
	zassert_ok(gpio_pin_configure(test_device, 0, GPIO_INPUT | GPIO_PULL_DOWN));
	zassert_equal(last_mask, BIT(0));
	zassert_equal(last_configuration, WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_PULL_DOWN);
	zassert_equal(pinctrl_calls, 4);
	zassert_equal(write_count, 0);
	zassert_equal(lock_calls, 0);
}

ZTEST(ws9652a_gpio, test_reads_physical_inputs_without_synthesizing_output_latches)
{
	gpio_port_value_t value;

	zassert_ok(gpio_pin_configure(test_device, 16, GPIO_OUTPUT));
	registers[GPIO_INPUT_VALUE / 4] = BIT(1) | BIT(16) | BIT(7) | BIT(24);
	registers[0] = BIT(0) | BIT(12);
	zassert_ok(gpio_port_get_raw(test_device, &value));
	zassert_equal(value, BIT(1) | BIT(16));
	zassert_equal(port_reads, 0);
	zassert_equal(write_count, 0);
	zassert_equal(lock_calls, 1);
	zassert_false(pad_locked);
}

ZTEST(ws9652a_gpio, test_toggle_uses_output_latches_and_aliases)
{
	registers[0] = BIT(0) | BIT(12);
	zassert_ok(gpio_port_toggle_bits(test_device, BIT(12) | BIT(16)));
	zassert_equal(port_reads, 1);
	zassert_equal(write_count, 2);
	zassert_equal(writes[0].address, ALIAS_BASE + 12u * 4u);
	zassert_equal(writes[0].value, 0);
	zassert_equal(writes[1].address, ALIAS_BASE + 16u * 4u);
	zassert_equal(writes[1].value, 1);
	zassert_equal(registers[0], BIT(0) | BIT(16));
	zassert_equal(lock_calls, 1);
	zassert_false(pad_locked);
}

ZTEST(ws9652a_gpio, test_unsupported_flags_have_no_side_effects)
{
	/* Test GPIO0.0: output plus pull-down is unsupported on the independent
	 * power latch, while ordinary output pins support that bias. */
	const gpio_flags_t unsupported[] = {
		GPIO_OUTPUT_HIGH | GPIO_OPEN_DRAIN, GPIO_OUTPUT_HIGH | GPIO_OPEN_SOURCE,
		GPIO_OUTPUT_HIGH | GPIO_PULL_UP,    GPIO_OUTPUT_HIGH | GPIO_PULL_DOWN,
		GPIO_INPUT | GPIO_OUTPUT_HIGH,      GPIO_DISCONNECTED,
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(unsupported); i++) {
		zassert_equal(ws9652a_gpio_configure(test_device, 0, unsupported[i]), -ENOTSUP);
	}
	zassert_equal(
		ws9652a_gpio_configure(test_device, 12, GPIO_OUTPUT_INIT_LOW | GPIO_OUTPUT_HIGH),
		-EINVAL);
	zassert_equal(ws9652a_gpio_configure(test_device, 1, GPIO_INPUT | GPIO_OUTPUT_INIT_LOW),
		      -EINVAL);
	zassert_equal(ws9652a_gpio_configure(test_device, 1, GPIO_INPUT | GPIO_PULL_UP), -ENOTSUP);
	zassert_equal(ws9652a_gpio_configure(test_device, 1,
					    GPIO_INPUT | GPIO_PULL_UP | GPIO_PULL_DOWN), -EINVAL);
	zassert_equal(ws9652a_gpio_configure(test_device, 0,
					    GPIO_OUTPUT_HIGH | GPIO_PULL_UP | GPIO_PULL_DOWN),
		      -EINVAL);
	zassert_equal(write_count, 0);
	zassert_equal(port_reads, 0);
	zassert_equal(pinctrl_calls, 0);
	zassert_equal(lock_calls, 0);
}

ZTEST_SUITE(ws9652a_gpio, NULL, NULL, reset, NULL, NULL);
