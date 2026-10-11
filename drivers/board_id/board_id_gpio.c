/* SPDX-License-Identifier: Apache-2.0 */
#define DT_DRV_COMPAT gpio_board_id

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/board_id.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

struct board_id_gpio_config {
	const struct gpio_dt_spec *gpios;
	const uint32_t *id_values;
	uint32_t sample_interval_us;
	uint8_t pin_count;
	uint8_t sample_count;
};

struct board_id_gpio_data {
	uint32_t id;
};

static int board_id_gpio_get(const struct device *dev, uint32_t *id)
{
	const struct board_id_gpio_data *data = dev->data;

	*id = data->id;
	return 0;
}

static int board_id_gpio_init(const struct device *dev)
{
	const struct board_id_gpio_config *config = dev->config;
	struct board_id_gpio_data *data = dev->data;
	uint32_t raw = 0;
	int ret;

	for (unsigned int pin = 0; pin < config->pin_count; pin++) {
		if (!gpio_is_ready_dt(&config->gpios[pin])) {
			return -ENODEV;
		}
		/* Bias comes from the GPIO specifier; externally biased straps need
		 * neither GPIO_PULL_UP nor GPIO_PULL_DOWN.
		 */
		ret = gpio_pin_configure_dt(&config->gpios[pin], GPIO_INPUT);
		if (ret < 0) {
			return ret;
		}
	}

	for (unsigned int pin = 0; pin < config->pin_count; pin++) {
		int value = gpio_pin_get_dt(&config->gpios[pin]);

		if (value < 0) {
			return value;
		}
		for (unsigned int sample = 1; sample < config->sample_count; sample++) {
			if (config->sample_interval_us != 0) {
				k_busy_wait(config->sample_interval_us);
			}
			ret = gpio_pin_get_dt(&config->gpios[pin]);
			if (ret < 0) {
				return ret;
			}
			if (ret != value) {
				return -EAGAIN;
			}
		}
		raw |= (uint32_t)value << pin;
	}

	data->id = config->id_values != NULL ? config->id_values[raw] : raw;
	return 0;
}

static DEVICE_API(board_id, board_id_gpio_api) = {
	.get = board_id_gpio_get,
};

#define BOARD_ID_GPIO_SPEC(node_id, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),

#define BOARD_ID_GPIO_VALUES_DEFINE(inst)                                                          \
	BUILD_ASSERT(DT_INST_PROP_LEN(inst, id_values) == BIT64(DT_INST_PROP_LEN(inst, gpios)),       \
		     "gpio-board-id id-values must contain one entry per GPIO bit pattern");        \
	static const uint32_t board_id_gpio_values_##inst[] = DT_INST_PROP(inst, id_values);

#define BOARD_ID_GPIO_DEFINE(inst)                                                                 \
	BUILD_ASSERT(DT_INST_PROP_LEN(inst, gpios) >= 1 && DT_INST_PROP_LEN(inst, gpios) <= 32,       \
		     "gpio-board-id requires 1 to 32 GPIOs");                                       \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, id_values),                                          \
		    (BOARD_ID_GPIO_VALUES_DEFINE(inst)), ())                                        \
	static const struct gpio_dt_spec board_id_gpio_pins_##inst[] = {                            \
		DT_INST_FOREACH_PROP_ELEM(inst, gpios, BOARD_ID_GPIO_SPEC)};                         \
	static const struct board_id_gpio_config board_id_gpio_config_##inst = {                   \
		.gpios = board_id_gpio_pins_##inst,                                                 \
		.id_values = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, id_values),                     \
					(board_id_gpio_values_##inst), (NULL)),                     \
		.sample_interval_us = DT_INST_PROP(inst, sample_interval_us),                       \
		.pin_count = DT_INST_PROP_LEN(inst, gpios),                                         \
		.sample_count = DT_INST_PROP(inst, sample_count),                                   \
	};                                                                                        \
	static struct board_id_gpio_data board_id_gpio_data_##inst;                                 \
	DEVICE_DT_INST_DEFINE(inst, board_id_gpio_init, NULL, &board_id_gpio_data_##inst,            \
			      &board_id_gpio_config_##inst, POST_KERNEL,                            \
			      CONFIG_BOARD_ID_GPIO_INIT_PRIORITY, &board_id_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(BOARD_ID_GPIO_DEFINE)
