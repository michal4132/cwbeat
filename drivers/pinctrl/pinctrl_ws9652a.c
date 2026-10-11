/* SPDX-License-Identifier: Apache-2.0 */
/* Verified TOP pad sequences. GPIO +0x0c enables a 22 kOhm pull-down per pin. */
#define DT_DRV_COMPAT zgmicro_ws9652a_pinctrl
#include <errno.h>
#include <zephyr/drivers/pinctrl/ws9652a.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "../gpio/zgmicro/gpio_ws9652a.h"

#define GPIO_OUTPUT_VALUE     0x00u
#define GPIO_MODE             0x04u
#define GPIO_CONFIG08         0x08u
#define GPIO_PULL_DOWN_ENABLE 0x0cu
#define OUTPUT_FLAGS                                                                               \
	(WS9652A_PINCFG_OUTPUT | WS9652A_PINCFG_OUTPUT_LOW | WS9652A_PINCFG_OUTPUT_HIGH)
#define BIAS_FLAGS   (WS9652A_PINCFG_BIAS_DISABLE | WS9652A_PINCFG_BIAS_PULL_DOWN)
#define CONFIG_FLAGS (WS9652A_PINCFG_INPUT | OUTPUT_FLAGS | BIAS_FLAGS)
#define I2C0_MASK     (BIT(7) | BIT(8))

struct k_spinlock ws9652a_pad_lock;

static void pad_modify(uintptr_t address, uint32_t mask, uint32_t value)
{
	sys_write32((sys_read32(address) & ~mask) | (value & mask), address);
}

static uintptr_t bank_address(unsigned bank)
{
	const uintptr_t ports[] = {DT_INST_REG_ADDR_BY_NAME(0, bank0),
				   DT_INST_REG_ADDR_BY_NAME(0, bank1)};
	return ports[bank];
}

static uintptr_t gpio_address(unsigned bank)
{
	const uintptr_t ports[] = {DT_INST_REG_ADDR_BY_NAME(0, gpio0),
				   DT_INST_REG_ADDR_BY_NAME(0, gpio1)};
	return ports[bank];
}

static uintptr_t output_address(unsigned bank)
{
	const uintptr_t ports[] = {DT_INST_REG_ADDR_BY_NAME(0, pin_output0),
				   DT_INST_REG_ADDR_BY_NAME(0, pin_output1)};
	return ports[bank];
}

static void route_gpio(unsigned bank, uint32_t mask)
{
	uintptr_t port = bank_address(bank);
	pad_modify(port, mask, mask);
	if (bank == 0) {
		pad_modify(port + 0x1c, mask, 0);
		pad_modify(port + 0x24, mask, 0);
		pad_modify(port + 0x28, mask, 0);
	} else {
		pad_modify(port + 0x0c, mask, 0);
		uint32_t old = sys_read32(port + 0x10);
		if (old & mask) {
			sys_write32(old & ~mask, port + 0x10);
		}
	}
}

static int validate_gpio(unsigned bank, uint32_t mask, uint32_t config)
{
	if (bank > 1 || !mask || (config & ~CONFIG_FLAGS)) {
		return -EINVAL;
	}
	if (((config & WS9652A_PINCFG_INPUT) && (config & OUTPUT_FLAGS)) ||
	    (config & (WS9652A_PINCFG_OUTPUT_LOW | WS9652A_PINCFG_OUTPUT_HIGH)) ==
		    (WS9652A_PINCFG_OUTPUT_LOW | WS9652A_PINCFG_OUTPUT_HIGH) ||
	    (config & BIAS_FLAGS) == BIAS_FLAGS) {
		return -EINVAL;
	}
	if (bank == 0 && (mask & BIT(0)) && (config & OUTPUT_FLAGS) && (config & BIAS_FLAGS)) {
		/* GPIO0.0 output controls the independent power latch. Its input
		 * bias must be configured separately, without output flags. */
		return -ENOTSUP;
	}
	return 0;
}

/* Caller holds ws9652a_pad_lock. Explicit output levels are applied separately. */
static void configure_gpio(unsigned bank, uint32_t mask, uint32_t config)
{
	uintptr_t port = gpio_address(bank);

	if (config & WS9652A_PINCFG_INPUT) {
		uint32_t levels = mask;
		if (bank == 0 && (mask & BIT(0))) {
			/* Routing can affect the readable latch: capture power hold first. */
			levels = (levels & ~BIT(0)) |
				 (sys_read32(port + GPIO_OUTPUT_VALUE) & BIT(0));
		}
		route_gpio(bank, mask);
		pad_modify(port + GPIO_MODE, mask, mask);
		ws9652a_gpio_latches_write(output_address(bank), mask, levels);
		pad_modify(port + GPIO_PULL_DOWN_ENABLE, mask,
			   config & WS9652A_PINCFG_BIAS_PULL_DOWN ? mask : 0);
		/* Keep the stock polarity field clear. DTS active-low conversion
		 * belongs to Zephyr's logical GPIO API, not the raw hardware path. */
		pad_modify(port + GPIO_CONFIG08, mask, 0);
	} else if (config & OUTPUT_FLAGS) {
		/* GPIO0.0's output latch is independent of its power-button input. */
		uint32_t pads = bank == 0 ? mask & ~BIT(0) : mask;
		if (pads) {
			route_gpio(bank, pads);
			pad_modify(port + GPIO_MODE, pads, 0);
			pad_modify(port + GPIO_PULL_DOWN_ENABLE, pads,
				   config & WS9652A_PINCFG_BIAS_PULL_DOWN ? pads : 0);
		}
	} else {
		bool power_pin = bank == 0 && (mask & BIT(0));
		uint32_t power_latch = power_pin ? sys_read32(port + GPIO_OUTPUT_VALUE) & BIT(0) : 0;

		route_gpio(bank, mask);
		if (power_pin) {
			ws9652a_gpio_latches_write(output_address(bank), BIT(0), power_latch);
		}
		if (config & BIAS_FLAGS) {
			pad_modify(port + GPIO_PULL_DOWN_ENABLE, mask,
				   config & WS9652A_PINCFG_BIAS_PULL_DOWN ? mask : 0);
		}
	}
}

static void configure_output_level(unsigned bank, uint32_t mask, uint32_t config)
{
	if (config & (WS9652A_PINCFG_OUTPUT_LOW | WS9652A_PINCFG_OUTPUT_HIGH)) {
		ws9652a_gpio_latches_write(output_address(bank), mask,
					   config & WS9652A_PINCFG_OUTPUT_HIGH ? mask : 0);
	}
}

int ws9652a_pinctrl_gpio_configure(unsigned bank, uint32_t mask, uint32_t config)
{
	int result = validate_gpio(bank, mask, config);
	if (result) {
		return result;
	}
	k_spinlock_key_t key = k_spin_lock(&ws9652a_pad_lock);
	configure_gpio(bank, mask, config);
	configure_output_level(bank, mask, config);
	k_spin_unlock(&ws9652a_pad_lock, key);
	return 0;
}

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt, uintptr_t reg)
{
	ARG_UNUSED(reg);
	uint32_t i2c_mask = 0;
	uint32_t gpio_i2c_mask = 0;
	if (pin_cnt && !pins) {
		return -EINVAL;
	}
	/* Preflight every entry before the first register access. */
	for (uint8_t i = 0; i < pin_cnt; ++i) {
		uint32_t mux = pins[i].pinmux;
		if ((mux & 0xc0u) != 0) {
			return -EINVAL;
		}
		unsigned function = mux >> 8;
		unsigned bank = (mux >> 5) & 1u, pin = mux & 0x1fu;
		if (function == WS9652A_FUNC_I2C0) {
			if (bank || (pin != 7 && pin != 8)) {
				return -EINVAL;
			}
			if (pins[i].config) {
				return -ENOTSUP;
			}
			i2c_mask |= BIT(pin);
		} else if (function == WS9652A_FUNC_GPIO) {
			int result = validate_gpio(bank, BIT(pin), pins[i].config);
			if (result) {
				return result;
			}
			if (bank == 0) {
				gpio_i2c_mask |= BIT(pin) & I2C0_MASK;
			}
		} else {
			return -ENOTSUP;
		}
	}
	if (i2c_mask && i2c_mask != I2C0_MASK) {
		return -EINVAL;
	}
	if (i2c_mask & gpio_i2c_mask) {
		/* The recovered peripheral mux is one indivisible two-pin operation. */
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&ws9652a_pad_lock);
	for (unsigned i = 0; i < pin_cnt;) {
		unsigned bank = (pins[i].pinmux >> 5) & 1u;
		uint32_t config = pins[i].config;
		uint32_t mask = BIT(pins[i].pinmux & 0x1fu);
		if ((pins[i].pinmux >> 8) == WS9652A_FUNC_I2C0) {
			/* Apply the verified pair once, at its first declaration. */
			if (i2c_mask) {
				pad_modify(bank_address(0) + 0x24, i2c_mask, i2c_mask);
				pad_modify(bank_address(0) + 0x28, i2c_mask, 0);
				i2c_mask = 0;
			}
			++i;
			continue;
		}
		++i;
		/* Adjacent pins sharing a profile use the recovered grouped write order. */
		while (i < pin_cnt && (pins[i].pinmux >> 8) == WS9652A_FUNC_GPIO &&
		       ((pins[i].pinmux >> 5) & 1u) == bank && pins[i].config == config) {
			mask |= BIT(pins[i++].pinmux & 0x1fu);
		}
		configure_gpio(bank, mask, config);
	}
	/* Keep explicit levels after every routing/profile write, like stock startup. */
	for (unsigned i = 0; i < pin_cnt; ++i) {
		bool superseded = false;
		for (unsigned next = i + 1; next < pin_cnt; ++next) {
			if ((pins[i].pinmux & 0x3fu) == (pins[next].pinmux & 0x3fu)) {
				superseded = true;
				break;
			}
		}
		if (!superseded) {
			configure_output_level((pins[i].pinmux >> 5) & 1u,
					       BIT(pins[i].pinmux & 0x1fu), pins[i].config);
		}
	}
	k_spin_unlock(&ws9652a_pad_lock, key);
	return 0;
}
