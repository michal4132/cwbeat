/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/ztest.h>
#include "private.h"

struct test_gpio_data {
	struct gpio_driver_data common;
	uint32_t latch;
	uint32_t outputs;
	unsigned configure_calls;
	int configure_error;
};

/* Model the GPIO API's latch contract. The WS9652A driver fixture separately
 * verifies that ordinary output configuration preserves actual MMIO latches. */
static int configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	struct test_gpio_data *data = dev->data;

	++data->configure_calls;
	if (data->configure_error) {
		return data->configure_error;
	}
	zassert_true(flags & GPIO_OUTPUT);
	data->outputs |= BIT(pin);
	if (flags & GPIO_OUTPUT_INIT_HIGH) {
		data->latch |= BIT(pin);
	} else if (flags & GPIO_OUTPUT_INIT_LOW) {
		data->latch &= ~BIT(pin);
	}
	return 0;
}

static DEVICE_API(gpio, test_gpio_api) = {
	.pin_configure = configure,
};

static const struct gpio_driver_config config = {
	.port_pin_mask = BIT(12) | BIT(16),
};
static struct test_gpio_data codec_data, type1_data;

DEVICE_DT_DEFINE(DT_NODELABEL(codec_gpio), NULL, NULL, &codec_data, &config, POST_KERNEL,
		 CONFIG_GPIO_INIT_PRIORITY, &test_gpio_api);
DEVICE_DT_DEFINE(DT_NODELABEL(type1_gpio), NULL, NULL, &type1_data, &config, POST_KERNEL,
		 CONFIG_GPIO_INIT_PRIORITY, &test_gpio_api);

static const struct device *const codec = DEVICE_DT_GET(DT_NODELABEL(codec_gpio));
static const struct device *const type1 = DEVICE_DT_GET(DT_NODELABEL(type1_gpio));

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	codec_data = (struct test_gpio_data){.latch = UINT32_MAX};
	type1_data = (struct test_gpio_data){.latch = UINT32_MAX};
	codec->state->init_res = 0;
	type1->state->init_res = 0;
}

ZTEST(audio_hardware, test_prepare_lowers_codec_and_preserves_type1_latch)
{
	const uint32_t inherited[] = {0, BIT(12), UINT32_MAX};

	for (unsigned i = 0; i < ARRAY_SIZE(inherited); ++i) {
		codec_data.latch = UINT32_MAX;
		type1_data.latch = inherited[i];
		zassert_ok(audio_hardware_prepare());
		zassert_equal(codec_data.latch, UINT32_MAX & ~BIT(16));
		zassert_equal(type1_data.latch, inherited[i]);
		zassert_equal(codec_data.outputs, BIT(16));
		zassert_equal(type1_data.outputs, BIT(12));
	}
}

ZTEST(audio_hardware, test_unready_control_does_not_change_either_pin)
{
	const struct device *const controls[] = {codec, type1};

	for (unsigned i = 0; i < ARRAY_SIZE(controls); ++i) {
		controls[i]->state->init_res = EIO;
		zassert_equal(audio_hardware_prepare(), -ENODEV);
		zassert_equal(codec_data.configure_calls, 0);
		zassert_equal(type1_data.configure_calls, 0);
		zassert_equal(codec_data.latch, UINT32_MAX);
		zassert_equal(type1_data.latch, UINT32_MAX);
		controls[i]->state->init_res = 0;
	}
}

ZTEST(audio_hardware, test_codec_configuration_error_stops_preparation)
{
	codec_data.configure_error = -EIO;
	zassert_equal(audio_hardware_prepare(), -EIO);
	zassert_equal(codec_data.configure_calls, 1);
	zassert_equal(type1_data.configure_calls, 0);
	zassert_equal(type1_data.latch, UINT32_MAX);
}

ZTEST(audio_hardware, test_type1_configuration_error_keeps_codec_low)
{
	type1_data.configure_error = -ENOTSUP;
	zassert_equal(audio_hardware_prepare(), -ENOTSUP);
	zassert_equal(codec_data.latch, UINT32_MAX & ~BIT(16));
	zassert_equal(type1_data.latch, UINT32_MAX);
}

ZTEST_SUITE(audio_hardware, NULL, NULL, reset, NULL, NULL);
