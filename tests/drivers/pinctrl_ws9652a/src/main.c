/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define TEST_BANK0    0x1000u
#define TEST_BANK1    0x1040u
#define TEST_ALIASES0 0x2000u
#define TEST_ALIASES1 0x2080u
#define PIN(bank, number, function, flags)                                                         \
	{.pinmux = WS9652A_PINMUX(bank, number, function), .config = (flags)}
static uint32_t registers[32];
static struct {
	uintptr_t address;
	uint32_t value;
} writes[96];
static unsigned int write_count, read_count, latch_reads, latch_reads_at_first_route;
static bool change_power_latch_on_route;

static uint32_t mock_read(uintptr_t address)
{
	zassert_true(address >= TEST_BANK0 && address < TEST_BANK0 + sizeof(registers));
	++read_count;
	if (address == TEST_BANK0) {
		++latch_reads;
	}
	return registers[(address - TEST_BANK0) / 4u];
}

static void mock_write(uint32_t value, uintptr_t address)
{
	zassert_true(write_count < ARRAY_SIZE(writes));
	writes[write_count].address = address;
	writes[write_count++].value = value;
	if (address >= TEST_ALIASES0 && address < TEST_ALIASES1 + 0x80u) {
		unsigned bank = address >= TEST_ALIASES1;
		uintptr_t aliases = bank ? TEST_ALIASES1 : TEST_ALIASES0;
		uint32_t mask = BIT((address - aliases) / 4u);
		registers[bank * 16] = (registers[bank * 16] & ~mask) | (value ? mask : 0);
		return;
	}
	zassert_true(address >= TEST_BANK0 && address < TEST_BANK0 + sizeof(registers));
	if (address == TEST_BANK0 + 0x14u) {
		latch_reads_at_first_route = latch_reads;
		if (change_power_latch_on_route) {
			registers[0] ^= BIT(0);
		}
	}
	registers[(address - TEST_BANK0) / 4u] = value;
}

#define sys_read32  mock_read
#define sys_write32 mock_write
#include "../../../../drivers/pinctrl/pinctrl_ws9652a.c"

PINCTRL_DT_DEFINE(DT_NODELABEL(test_consumer));
PINCTRL_DT_DEFINE(DT_NODELABEL(test_output_consumer));

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	memset(registers, 0xff, sizeof(registers));
	write_count = read_count = latch_reads = latch_reads_at_first_route = 0;
	change_power_latch_on_route = false;
}

ZTEST(ws9652a_pinctrl, test_bank0_routes_without_changing_latch_or_direction)
{
	const pinctrl_soc_pin_t pins[] = {PIN(0, 12, WS9652A_FUNC_GPIO, 0)};
	const uint32_t offsets[] = {0x14, 0x30, 0x38, 0x3c};
	zassert_ok(pinctrl_configure_pins(pins, ARRAY_SIZE(pins), PINCTRL_REG_NONE));
	zassert_equal(write_count, ARRAY_SIZE(offsets));
	for (unsigned int i = 0; i < ARRAY_SIZE(offsets); ++i) {
		zassert_equal(writes[i].address, TEST_BANK0 + offsets[i]);
		zassert_equal(writes[i].value & ~BIT(12), UINT32_MAX & ~BIT(12));
	}
	zassert_equal(registers[0], UINT32_MAX);
	zassert_equal(registers[1], UINT32_MAX);
	zassert_equal(registers[2], UINT32_MAX);
	zassert_equal(registers[3], UINT32_MAX);
}

ZTEST(ws9652a_pinctrl, test_bank1_selects_its_own_register_profile)
{
	const pinctrl_soc_pin_t pins[] = {PIN(1, 16, WS9652A_FUNC_GPIO, 0)};
	const uint32_t offsets[] = {0x14, 0x20, 0x24};
	zassert_ok(pinctrl_configure_pins(pins, ARRAY_SIZE(pins), PINCTRL_REG_NONE));
	zassert_equal(write_count, ARRAY_SIZE(offsets));
	for (unsigned int i = 0; i < ARRAY_SIZE(offsets); ++i) {
		zassert_equal(writes[i].address, TEST_BANK1 + offsets[i]);
		zassert_equal(writes[i].value & ~BIT(16), UINT32_MAX & ~BIT(16));
	}
	zassert_equal(registers[16], UINT32_MAX);
	zassert_equal(registers[17], UINT32_MAX);
	zassert_equal(registers[18], UINT32_MAX);
	zassert_equal(registers[19], UINT32_MAX);
}

ZTEST(ws9652a_pinctrl, test_invalid_state_has_no_partial_writes)
{
	pinctrl_soc_pin_t pins[] = {
		PIN(0, 12, WS9652A_FUNC_GPIO, WS9652A_PINCFG_OUTPUT_LOW),
		PIN(0, 18, 2, 0),
	};
	zassert_equal(pinctrl_configure_pins(pins, ARRAY_SIZE(pins), 0), -ENOTSUP);
	pins[1].pinmux = WS9652A_PINMUX(2, 1, WS9652A_FUNC_GPIO);
	zassert_equal(pinctrl_configure_pins(pins, ARRAY_SIZE(pins), 0), -EINVAL);
	pins[1].pinmux = WS9652A_PINMUX(0, 32, WS9652A_FUNC_GPIO);
	zassert_equal(pinctrl_configure_pins(pins, ARRAY_SIZE(pins), 0), -EINVAL);
	pins[1].pinmux = WS9652A_PINMUX(0, 1, WS9652A_FUNC_GPIO);
	const uint32_t invalid_configs[] = {
		WS9652A_PINCFG_INPUT | WS9652A_PINCFG_OUTPUT,
		WS9652A_PINCFG_INPUT | WS9652A_PINCFG_OUTPUT_LOW,
		WS9652A_PINCFG_OUTPUT_LOW | WS9652A_PINCFG_OUTPUT_HIGH,
		WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_DISABLE | WS9652A_PINCFG_BIAS_PULL_DOWN,
		WS9652A_PINCFG_BIAS_DISABLE | WS9652A_PINCFG_BIAS_PULL_DOWN,
		BIT(31),
	};
	for (unsigned i = 0; i < ARRAY_SIZE(invalid_configs); ++i) {
		pins[1].config = invalid_configs[i];
		zassert_equal(pinctrl_configure_pins(pins, ARRAY_SIZE(pins), 0), -EINVAL);
	}
	pins[1].pinmux = WS9652A_PINMUX(0, 0, WS9652A_FUNC_GPIO);
	pins[1].config = WS9652A_PINCFG_OUTPUT | WS9652A_PINCFG_BIAS_DISABLE;
	zassert_equal(pinctrl_configure_pins(pins, ARRAY_SIZE(pins), 0), -ENOTSUP);
	pins[1].config = WS9652A_PINCFG_OUTPUT_HIGH | WS9652A_PINCFG_BIAS_PULL_DOWN;
	zassert_equal(pinctrl_configure_pins(pins, ARRAY_SIZE(pins), 0), -ENOTSUP);
	zassert_equal(pinctrl_configure_pins(NULL, 1, 0), -EINVAL);
	zassert_equal(write_count, 0);
	zassert_equal(read_count, 0);
}

ZTEST(ws9652a_pinctrl, test_bank1_group_skips_unchanged_config24)
{
	registers[(0x40 + 0x24) / 4] = 0;
	zassert_ok(ws9652a_pinctrl_gpio_configure(1, BIT(1) | BIT(4), 0));
	zassert_equal(write_count, 2);
	zassert_equal(writes[0].address, TEST_BANK1 + 0x14);
	zassert_equal(writes[1].address, TEST_BANK1 + 0x20);
}

ZTEST(ws9652a_pinctrl, test_i2c_group_only_changes_mux)
{
	const pinctrl_soc_pin_t pins[] = {PIN(0, 7, WS9652A_FUNC_I2C0, 0),
					  PIN(0, 8, WS9652A_FUNC_I2C0, 0)};
	zassert_equal(pinctrl_configure_pins(pins, 1, 0), -EINVAL);
	zassert_equal(write_count, 0);
	zassert_ok(pinctrl_configure_pins(pins, 2, 0));
	zassert_equal(write_count, 2);
	zassert_equal(writes[0].address, TEST_BANK0 + 0x38);
	zassert_equal(writes[1].address, TEST_BANK0 + 0x3c);
	zassert_equal(writes[1].value, UINT32_MAX & ~(BIT(7) | BIT(8)));
	zassert_equal(registers[0], UINT32_MAX);
}

ZTEST(ws9652a_pinctrl, test_i2c_pin_config_and_mixed_routing_rejected)
{
	pinctrl_soc_pin_t pins[] = {
		PIN(0, 7, WS9652A_FUNC_I2C0, WS9652A_PINCFG_OUTPUT_LOW),
		PIN(0, 8, WS9652A_FUNC_I2C0, 0),
		PIN(0, 7, WS9652A_FUNC_GPIO, 0),
	};
	zassert_equal(pinctrl_configure_pins(pins, 2, 0), -ENOTSUP);
	pins[0].config = 0;
	zassert_equal(pinctrl_configure_pins(pins, 3, 0), -EINVAL);
	zassert_equal(write_count, 0);
}

ZTEST(ws9652a_pinctrl, test_dt_state_preserves_group_order_and_defers_output_level)
{
	const struct pinctrl_dev_config *config =
		PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(test_consumer));
	const struct pinctrl_state *state;
	zassert_ok(pinctrl_lookup_state(config, PINCTRL_STATE_DEFAULT, &state));
	zassert_equal(state->pin_cnt, 5);
	zassert_equal(state->pins[0].config, WS9652A_PINCFG_INPUT);
	zassert_equal(state->pins[1].config, WS9652A_PINCFG_OUTPUT_LOW);
	zassert_equal(state->pins[2].config, WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_PULL_DOWN);
	zassert_equal(state->pins[4].config, WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_DISABLE);
	registers[0] = BIT(10) | BIT(12) | BIT(16) | BIT(18);
	registers[16] = BIT(4) | BIT(16);
	zassert_ok(pinctrl_apply_state(config, PINCTRL_STATE_DEFAULT));

	/* Bank1 input, bank0 output, two-pin input group, then repeated pin9 input. */
	zassert_equal(write_count, 31);
	zassert_equal(writes[0].address, TEST_BANK1 + 0x14);
	zassert_equal(writes[7].address, TEST_BANK0 + 0x14);
	zassert_equal(writes[13].address, TEST_BANK0 + 0x14);
	zassert_equal(writes[13].value & (BIT(9) | BIT(11)), BIT(9) | BIT(11));
	zassert_equal(writes[18].address, TEST_ALIASES0 + 9 * 4);
	zassert_equal(writes[19].address, TEST_ALIASES0 + 11 * 4);
	zassert_equal(writes[20].address, TEST_BANK0 + GPIO_PULL_DOWN_ENABLE);
	zassert_equal(writes[20].value & (BIT(9) | BIT(11)), BIT(9) | BIT(11));
	zassert_equal(writes[22].address, TEST_BANK0 + 0x14);
	zassert_equal(writes[28].address, TEST_BANK0 + GPIO_PULL_DOWN_ENABLE);
	zassert_equal(writes[28].value & (BIT(9) | BIT(11)), BIT(11));
	zassert_equal(writes[30].address, TEST_ALIASES0 + 10 * 4);
	zassert_equal(writes[30].value, 0);
	zassert_equal(registers[0], BIT(9) | BIT(11) | BIT(12) | BIT(16) | BIT(18));
	zassert_equal(registers[16], BIT(1) | BIT(4) | BIT(16));
	zassert_equal(registers[(0x40 + GPIO_PULL_DOWN_ENABLE) / 4], UINT32_MAX & ~BIT(1));
	zassert_equal(registers[GPIO_MODE / 4], UINT32_MAX & ~BIT(10));
	zassert_equal(registers[GPIO_CONFIG08 / 4], UINT32_MAX & ~(BIT(9) | BIT(11)));
	zassert_equal(latch_reads, 0);
}

ZTEST(ws9652a_pinctrl, test_dt_output_enable_preserves_latch_and_high_sets_late)
{
	const struct pinctrl_dev_config *config =
		PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(test_output_consumer));
	registers[0] = BIT(12);
	zassert_ok(pinctrl_apply_state(config, PINCTRL_STATE_DEFAULT));
	zassert_equal(write_count, 13);
	zassert_equal(writes[12].address, TEST_ALIASES0 + 16 * 4);
	zassert_equal(writes[12].value, 1);
	zassert_equal(registers[0], BIT(12) | BIT(16));
	zassert_equal(registers[GPIO_PULL_DOWN_ENABLE / 4] & (BIT(12) | BIT(16)), BIT(16));
}

ZTEST(ws9652a_pinctrl, test_both_banks_set_and_clear_input_pull_down)
{
	const uint32_t mask = BIT(5) | BIT(6);

	for (unsigned bank = 0; bank < 2; ++bank) {
		unsigned pull_reg = (bank * 0x40 + GPIO_PULL_DOWN_ENABLE) / 4;
		uint32_t unrelated = 0xa55aa55au & ~mask;

		registers[pull_reg] = unrelated;
		write_count = 0;
		zassert_ok(ws9652a_pinctrl_gpio_configure(
			bank, mask, WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_PULL_DOWN));
		zassert_equal(registers[pull_reg], unrelated | mask);
		zassert_equal(registers[bank * 16 + GPIO_MODE / 4] & mask, mask);
		zassert_ok(ws9652a_pinctrl_gpio_configure(bank, mask, WS9652A_PINCFG_INPUT));
		zassert_equal(registers[pull_reg], unrelated);
		zassert_ok(ws9652a_pinctrl_gpio_configure(
			bank, mask, WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_PULL_DOWN));
		zassert_ok(ws9652a_pinctrl_gpio_configure(
			bank, mask, WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_DISABLE));
		zassert_equal(registers[pull_reg], unrelated);
	}
}

ZTEST(ws9652a_pinctrl, test_output_pull_down_and_direction_transitions)
{
	const uint32_t mask = BIT(5);

	for (unsigned bank = 0; bank < 2; ++bank) {
		unsigned base = bank * 16;
		write_count = 0;
		zassert_ok(ws9652a_pinctrl_gpio_configure(
			bank, mask, WS9652A_PINCFG_OUTPUT_LOW | WS9652A_PINCFG_BIAS_PULL_DOWN));
		zassert_equal(registers[base + GPIO_PULL_DOWN_ENABLE / 4], UINT32_MAX);
		zassert_equal(registers[base + GPIO_MODE / 4], UINT32_MAX & ~mask);
		zassert_equal(registers[base], UINT32_MAX & ~mask);
		zassert_ok(ws9652a_pinctrl_gpio_configure(bank, mask, WS9652A_PINCFG_INPUT));
		zassert_equal(registers[base + GPIO_PULL_DOWN_ENABLE / 4], UINT32_MAX & ~mask);
		zassert_equal(registers[base + GPIO_MODE / 4], UINT32_MAX);
		zassert_ok(ws9652a_pinctrl_gpio_configure(
			bank, mask, WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_PULL_DOWN));
		zassert_ok(ws9652a_pinctrl_gpio_configure(bank, mask, WS9652A_PINCFG_OUTPUT));
		zassert_equal(registers[base + GPIO_PULL_DOWN_ENABLE / 4], UINT32_MAX & ~mask);
		zassert_equal(registers[base + GPIO_MODE / 4], UINT32_MAX & ~mask);
		zassert_equal(registers[base], UINT32_MAX);
	}
}

ZTEST(ws9652a_pinctrl, test_bias_only_preserves_direction_and_latches)
{
	const uint32_t mask = BIT(0) | BIT(5);
	change_power_latch_on_route = true;

	for (unsigned bank = 0; bank < 2; ++bank) {
		unsigned base = bank * 16;
		write_count = 0;
		registers[base] = 0xa55aa55au;
		registers[base + GPIO_MODE / 4] = 0x55aa55aau;
		zassert_ok(ws9652a_pinctrl_gpio_configure(bank, mask, WS9652A_PINCFG_BIAS_DISABLE));
		zassert_equal(registers[base + GPIO_PULL_DOWN_ENABLE / 4], UINT32_MAX & ~mask);
		zassert_ok(ws9652a_pinctrl_gpio_configure(bank, mask, WS9652A_PINCFG_BIAS_PULL_DOWN));
		zassert_equal(registers[base + GPIO_PULL_DOWN_ENABLE / 4], UINT32_MAX);
		zassert_equal(registers[base], 0xa55aa55au);
		zassert_equal(registers[base + GPIO_MODE / 4], 0x55aa55aau);
		if (bank == 0) {
			zassert_true(latch_reads_at_first_route > 0);
		}
	}
}

ZTEST(ws9652a_pinctrl, test_power_input_bias_preserves_latch_and_output_preserves_bias)
{
	for (unsigned high = 0; high <= 1; ++high) {
		write_count = 0;
		registers[0] = (UINT32_MAX & ~BIT(0)) | high;
		zassert_ok(ws9652a_pinctrl_gpio_configure(
			0, BIT(0), WS9652A_PINCFG_INPUT | WS9652A_PINCFG_BIAS_PULL_DOWN));
		zassert_equal(registers[0] & BIT(0), high);
		zassert_equal(registers[GPIO_PULL_DOWN_ENABLE / 4], UINT32_MAX);
		zassert_ok(ws9652a_pinctrl_gpio_configure(0, BIT(0), WS9652A_PINCFG_OUTPUT_LOW));
		zassert_equal(registers[GPIO_PULL_DOWN_ENABLE / 4], UINT32_MAX);
		zassert_equal(registers[GPIO_MODE / 4], UINT32_MAX);
		zassert_ok(ws9652a_pinctrl_gpio_configure(0, BIT(0), WS9652A_PINCFG_INPUT));
		zassert_equal(registers[0] & BIT(0), 0);
		zassert_equal(registers[GPIO_PULL_DOWN_ENABLE / 4], UINT32_MAX & ~BIT(0));
		zassert_ok(ws9652a_pinctrl_gpio_configure(0, BIT(0), WS9652A_PINCFG_OUTPUT_HIGH));
		zassert_equal(registers[0] & BIT(0), BIT(0));
		zassert_equal(registers[GPIO_PULL_DOWN_ENABLE / 4], UINT32_MAX & ~BIT(0));
		zassert_equal(registers[GPIO_MODE / 4], UINT32_MAX);
	}
}

ZTEST(ws9652a_pinctrl, test_later_input_cancels_pending_output_level)
{
	const pinctrl_soc_pin_t pins[] = {
		PIN(0, 12, WS9652A_FUNC_GPIO, WS9652A_PINCFG_OUTPUT_LOW),
		PIN(0, 12, WS9652A_FUNC_GPIO, WS9652A_PINCFG_INPUT),
	};
	zassert_ok(pinctrl_configure_pins(pins, ARRAY_SIZE(pins), 0));
	zassert_equal(write_count, 14);
	zassert_equal(registers[0] & BIT(12), BIT(12));
	zassert_equal(registers[GPIO_MODE / 4] & BIT(12), BIT(12));
	zassert_equal(writes[13].address, TEST_BANK0 + GPIO_CONFIG08);
}

ZTEST(ws9652a_pinctrl, test_power_input_snapshots_latch_before_routing)
{
	registers[0] &= ~BIT(0);
	change_power_latch_on_route = true;
	zassert_ok(ws9652a_pinctrl_gpio_configure(0, BIT(0) | BIT(1), WS9652A_PINCFG_INPUT));
	zassert_equal(latch_reads_at_first_route, 1);
	zassert_equal(writes[5].address, TEST_ALIASES0);
	zassert_equal(writes[5].value, 0);
	zassert_equal(writes[6].address, TEST_ALIASES0 + 4);
	zassert_equal(writes[6].value, 1);
	zassert_equal(registers[0] & BIT(0), 0);
}

ZTEST(ws9652a_pinctrl, test_power_output_only_changes_latch)
{
	zassert_ok(ws9652a_pinctrl_gpio_configure(0, BIT(0), WS9652A_PINCFG_OUTPUT_LOW));
	zassert_equal(write_count, 1);
	zassert_equal(writes[0].address, TEST_ALIASES0);
	zassert_equal(writes[0].value, 0);
	zassert_equal(registers[0], UINT32_MAX & ~BIT(0));
	zassert_equal(registers[GPIO_MODE / 4], UINT32_MAX);
	zassert_equal(registers[0x14 / 4], UINT32_MAX);
}

ZTEST(ws9652a_pinctrl, test_standalone_gpio_helper_validates_before_access)
{
	zassert_equal(ws9652a_pinctrl_gpio_configure(2, BIT(1), WS9652A_PINCFG_INPUT), -EINVAL);
	zassert_equal(ws9652a_pinctrl_gpio_configure(0, 0, WS9652A_PINCFG_INPUT), -EINVAL);
	zassert_equal(ws9652a_pinctrl_gpio_configure(0, BIT(0) | BIT(1),
			WS9652A_PINCFG_OUTPUT | WS9652A_PINCFG_BIAS_PULL_DOWN), -ENOTSUP);
	zassert_equal(write_count, 0);
	zassert_equal(read_count, 0);
}

ZTEST_SUITE(ws9652a_pinctrl, NULL, NULL, reset, NULL, NULL);
