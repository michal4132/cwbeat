/* SPDX-License-Identifier: Apache-2.0 */
/* GPIO API validation and runtime data access. Pad routing, direction and
 * configuration are owned by the WS9652A pinctrl provider. */
#define DT_DRV_COMPAT zgmicro_ws9652a_gpio

#include <errno.h>
#include <zephyr/drivers/pinctrl/ws9652a.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "gpio_ws9652a.h"

#define GPIO_OUTPUT_VALUE 0x00u
#define GPIO_INPUT_VALUE  0x10u

struct ws9652a_gpio_config {
	struct gpio_driver_config common;
	uintptr_t port;
	uintptr_t pin_output;
	uint8_t bank;
};

struct ws9652a_gpio_data {
	struct gpio_driver_data common;
};

static int ws9652a_gpio_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct ws9652a_gpio_config *cfg = dev->config;
	const gpio_flags_t supported = GPIO_INPUT | GPIO_OUTPUT | GPIO_ACTIVE_LOW |
				       GPIO_OUTPUT_INIT_LOW | GPIO_OUTPUT_INIT_HIGH | GPIO_PULL_DOWN;
	uint32_t mask;
	gpio_flags_t direction = flags & GPIO_DIR_MASK;

	if (pin >= GPIO_MAX_PINS_PER_PORT || (cfg->common.port_pin_mask & BIT(pin)) == 0u) {
		return -EINVAL;
	}
	mask = BIT(pin);
	if ((flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) == (GPIO_PULL_UP | GPIO_PULL_DOWN)) {
		return -EINVAL;
	}
	if ((flags & ~supported) != 0u || (direction != GPIO_INPUT && direction != GPIO_OUTPUT)) {
		return -ENOTSUP;
	}
	if (cfg->bank == 0 && pin == 0 && direction == GPIO_OUTPUT && (flags & GPIO_PULL_DOWN)) {
		return -ENOTSUP;
	}
	if ((flags & (GPIO_OUTPUT_INIT_LOW | GPIO_OUTPUT_INIT_HIGH)) ==
		    (GPIO_OUTPUT_INIT_LOW | GPIO_OUTPUT_INIT_HIGH) ||
	    (!(flags & GPIO_OUTPUT) && (flags & (GPIO_OUTPUT_INIT_LOW | GPIO_OUTPUT_INIT_HIGH)))) {
		return -EINVAL;
	}

	uint32_t configuration = flags & GPIO_INPUT ? WS9652A_PINCFG_INPUT : WS9652A_PINCFG_OUTPUT;
	if (flags & GPIO_PULL_DOWN) {
		configuration |= WS9652A_PINCFG_BIAS_PULL_DOWN;
	}
	if (flags & GPIO_OUTPUT_INIT_LOW) {
		configuration |= WS9652A_PINCFG_OUTPUT_LOW;
	} else if (flags & GPIO_OUTPUT_INIT_HIGH) {
		configuration |= WS9652A_PINCFG_OUTPUT_HIGH;
	}
	/* The provider serializes its complete pad transaction, including the
	 * power-latch special cases. Do not nest the shared lock here. */
	return ws9652a_pinctrl_gpio_configure(cfg->bank, mask, configuration);
}

static int ws9652a_gpio_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	const struct ws9652a_gpio_config *cfg = dev->config;

	/* Return physical levels. Zephyr records the DTS active-low flag during
	 * configuration and applies it once in logical reads such as gpio_pin_get_dt(). */
	k_spinlock_key_t key = k_spin_lock(&ws9652a_pad_lock);
	*value = sys_read32(cfg->port + GPIO_INPUT_VALUE) & cfg->common.port_pin_mask;
	k_spin_unlock(&ws9652a_pad_lock, key);
	return 0;
}

static int ws9652a_gpio_set_masked(const struct device *dev, gpio_port_pins_t mask,
				   gpio_port_value_t value)
{
	const struct ws9652a_gpio_config *cfg = dev->config;
	k_spinlock_key_t key;

	if ((mask & ~cfg->common.port_pin_mask) != 0u) {
		return -ENOTSUP;
	}
	key = k_spin_lock(&ws9652a_pad_lock);
	ws9652a_gpio_latches_write(cfg->pin_output, mask, value);
	k_spin_unlock(&ws9652a_pad_lock, key);
	return 0;
}

static int ws9652a_gpio_set_bits(const struct device *dev, gpio_port_pins_t pins)
{
	return ws9652a_gpio_set_masked(dev, pins, pins);
}

static int ws9652a_gpio_clear_bits(const struct device *dev, gpio_port_pins_t pins)
{
	return ws9652a_gpio_set_masked(dev, pins, 0u);
}

static int ws9652a_gpio_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	const struct ws9652a_gpio_config *cfg = dev->config;
	k_spinlock_key_t key;

	if ((pins & ~cfg->common.port_pin_mask) != 0u) {
		return -ENOTSUP;
	}
	key = k_spin_lock(&ws9652a_pad_lock);
	ws9652a_gpio_latches_write(cfg->pin_output, pins,
				  sys_read32(cfg->port + GPIO_OUTPUT_VALUE) ^ pins);
	k_spin_unlock(&ws9652a_pad_lock, key);
	return 0;
}

static int ws9652a_gpio_interrupt_configure(const struct device *dev, gpio_pin_t pin,
					    enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(pin);
	ARG_UNUSED(trig);
	return mode == GPIO_INT_MODE_DISABLED ? 0 : -ENOTSUP;
}

static DEVICE_API(gpio, ws9652a_gpio_api) = {
	.pin_configure = ws9652a_gpio_configure,
	.port_get_raw = ws9652a_gpio_get_raw,
	.port_set_masked_raw = ws9652a_gpio_set_masked,
	.port_set_bits_raw = ws9652a_gpio_set_bits,
	.port_clear_bits_raw = ws9652a_gpio_clear_bits,
	.port_toggle_bits = ws9652a_gpio_toggle_bits,
	.pin_interrupt_configure = ws9652a_gpio_interrupt_configure,
};

#define WS9652A_GPIO_DEFINE(inst)                                                                  \
	BUILD_ASSERT(DT_INST_REG_HAS_NAME(inst, pin_output), "GPIO needs output aliases");         \
	BUILD_ASSERT(DT_INST_PROP(inst, ngpios) > 0 && DT_INST_PROP(inst, ngpios) <= 32,           \
		     "WS9652A GPIO banks have at most 32 pins");                                   \
	static struct ws9652a_gpio_data gpio_data_##inst;                                          \
	static const struct ws9652a_gpio_config gpio_config_##inst = {                             \
		.common = GPIO_COMMON_CONFIG_FROM_DT_INST(inst),                                   \
		.port = DT_INST_REG_ADDR_BY_NAME(inst, port),                                      \
		.pin_output = DT_INST_REG_ADDR_BY_NAME(inst, pin_output),                          \
		.bank = DT_INST_PROP(inst, bank),                                                  \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &gpio_data_##inst, &gpio_config_##inst,            \
			      PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY, &ws9652a_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(WS9652A_GPIO_DEFINE)
