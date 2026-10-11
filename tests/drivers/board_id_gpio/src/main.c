/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/board_id.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/ztest.h>

/* The production board-ID device uses the real GPIO API against a scripted
 * controller. Errors and changing straps are deterministic, without hooks in
 * the board-ID driver or direct access to its private state. */
#define DT_DRV_COMPAT vnd_board_id_test_gpio

struct test_gpio_config {
	struct gpio_driver_config common;
	bool fail_init;
};

struct test_gpio_data {
	struct gpio_driver_data common;
	gpio_flags_t flags[32];
	gpio_port_value_t input;
	gpio_port_value_t sequence[3];
	unsigned int sequence_count;
	unsigned int configurations;
	unsigned int reads;
	int configure_result;
	int read_result;
};

enum event {
	CONFIGURE,
	READ
};
static enum event events[80];
static unsigned int event_count;

static void record_event(enum event event)
{
	if (event_count < ARRAY_SIZE(events)) {
		events[event_count++] = event;
	}
}

static int test_gpio_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	struct test_gpio_data *data = dev->data;

	record_event(CONFIGURE);
	data->configurations++;
	data->flags[pin] = flags;
	return data->configure_result;
}

static int test_gpio_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	struct test_gpio_data *data = dev->data;
	unsigned int index = data->reads++;

	record_event(READ);
	if (data->read_result) {
		return data->read_result;
	}
	*value = index < data->sequence_count ? data->sequence[index] : data->input;
	return 0;
}

static int test_gpio_init(const struct device *dev)
{
	const struct test_gpio_config *config = dev->config;

	return config->fail_init ? -ENODEV : 0;
}

static DEVICE_API(gpio, test_gpio_api) = {
	.pin_configure = test_gpio_configure,
	.port_get_raw = test_gpio_get_raw,
};

#define TEST_GPIO_DEFINE(inst)                                                                     \
	static struct test_gpio_data test_gpio_data_##inst;                                        \
	static const struct test_gpio_config test_gpio_config_##inst = {                           \
		.common = GPIO_COMMON_CONFIG_FROM_DT_INST(inst),                                   \
		.fail_init = DT_INST_PROP(inst, fail_init),                                        \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, test_gpio_init, NULL, &test_gpio_data_##inst,                  \
			      &test_gpio_config_##inst, PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY,   \
			      &test_gpio_api);
DT_INST_FOREACH_STATUS_OKAY(TEST_GPIO_DEFINE)

#define ID(name) DEVICE_DT_GET(DT_NODELABEL(name))
static const struct device *const port0 = DEVICE_DT_GET(DT_NODELABEL(test_gpio0));
static const struct device *const port1 = DEVICE_DT_GET(DT_NODELABEL(test_gpio1));

static struct test_gpio_data *gpio_data(const struct device *dev)
{
	return dev->data;
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	memset(gpio_data(port0), 0, sizeof(struct test_gpio_data));
	memset(gpio_data(port1), 0, sizeof(struct test_gpio_data));
	event_count = 0;
}

static void assert_id(const struct device *dev, uint32_t expected)
{
	uint32_t actual = UINT32_MAX;

	zassert_ok(board_id_get(dev, &actual));
	zassert_equal(actual, expected);
}

static void assert_failed_without_resampling(const struct device *dev, unsigned int reads)
{
	uint32_t value = 0x12345678;
	unsigned int configurations = gpio_data(port0)->configurations;

	zassert_false(device_is_ready(dev));
	zassert_equal(board_id_get(dev, &value), -ENODEV);
	zassert_equal(value, 0x12345678);
	zassert_equal(device_init(dev), -EALREADY);
	zassert_equal(gpio_data(port0)->reads, reads);
	zassert_equal(gpio_data(port0)->configurations, configurations);
}

ZTEST(board_id_gpio, test_automatic_initialization)
{
	zassert_true(device_is_ready(ID(auto_id)));
	assert_id(ID(auto_id), 0);
	zassert_equal(device_init(ID(auto_id)), -EALREADY);
	zassert_equal(event_count, 0);
}

ZTEST(board_id_gpio, test_deferred_raw_zero_and_argument_validation)
{
	uint32_t value = UINT32_MAX;

	zassert_equal(board_id_get(NULL, &value), -EINVAL);
	zassert_equal(board_id_get(ID(raw_zero_id), NULL), -EINVAL);
	zassert_false(device_is_ready(ID(raw_zero_id)));
	zassert_equal(board_id_get(ID(raw_zero_id), &value), -ENODEV);
	zassert_equal(value, UINT32_MAX);
	zassert_equal(event_count, 0);
	zassert_ok(device_init(ID(raw_zero_id)));
	assert_id(ID(raw_zero_id), 0);
	zassert_equal(gpio_data(port0)->configurations, 1);
	zassert_equal(gpio_data(port0)->flags[0], GPIO_INPUT);
	zassert_equal(gpio_data(port0)->reads, 1);
}

ZTEST(board_id_gpio, test_mapping_all_four_patterns)
{
	const struct device *const devices[] = {
		ID(mapped0_id),
		ID(mapped1_id),
		ID(mapped2_id),
		ID(mapped3_id),
	};
	const uint32_t expected[] = {4, 2, 1, 3};

	for (unsigned int i = 0; i < ARRAY_SIZE(devices); ++i) {
		gpio_data(port0)->input = i;
		zassert_ok(device_init(devices[i]));
		assert_id(devices[i], expected[i]);
	}
}

ZTEST(board_id_gpio, test_polarity_flags_and_mixed_controllers)
{
	gpio_data(port0)->input = 0;
	gpio_data(port1)->input = BIT(5);
	zassert_ok(device_init(ID(mixed_id)));
	assert_id(ID(mixed_id), 3);
	zassert_equal(gpio_data(port0)->flags[3] & GPIO_DIR_MASK, GPIO_INPUT);
	zassert_true(gpio_data(port0)->flags[3] & GPIO_PULL_UP);
	zassert_equal(gpio_data(port1)->flags[5] & GPIO_DIR_MASK, GPIO_INPUT);
	zassert_true(gpio_data(port1)->flags[5] & GPIO_PULL_DOWN);
	zassert_equal(event_count, 4);
	zassert_equal(events[0], CONFIGURE);
	zassert_equal(events[1], CONFIGURE);
	zassert_equal(events[2], READ);
	zassert_equal(events[3], READ);
}

ZTEST(board_id_gpio, test_external_straps_request_no_pull_before_sampling)
{
	gpio_data(port0)->input = BIT(21);
	zassert_ok(device_init(ID(external_straps_id)));
	assert_id(ID(external_straps_id), 1);
	zassert_equal(gpio_data(port0)->flags[17], GPIO_INPUT);
	zassert_equal(gpio_data(port0)->flags[21], GPIO_INPUT);
	zassert_equal(gpio_data(port0)->configurations, 2);
	zassert_equal(gpio_data(port0)->reads, 6);
	zassert_equal(event_count, 8);
	zassert_equal(events[0], CONFIGURE);
	zassert_equal(events[1], CONFIGURE);
	for (unsigned int i = 2; i < event_count; ++i) {
		zassert_equal(events[i], READ);
	}
}

ZTEST(board_id_gpio, test_full_32_bit_identifier)
{
	gpio_data(port0)->input = 0xa5a55a5a;
	zassert_ok(device_init(ID(raw32_id)));
	assert_id(ID(raw32_id), 0xa5a55a5a);
	zassert_equal(gpio_data(port0)->configurations, 32);
	zassert_equal(gpio_data(port0)->reads, 32);
}

ZTEST(board_id_gpio, test_stable_sampling_caches_identity)
{
	gpio_data(port0)->input = BIT(4);
	zassert_ok(device_init(ID(cached_id)));
	assert_id(ID(cached_id), 1);
	zassert_equal(gpio_data(port0)->reads, 3);
	gpio_data(port0)->input = 0;
	assert_id(ID(cached_id), 1);
	zassert_equal(device_init(ID(cached_id)), -EALREADY);
	zassert_equal(gpio_data(port0)->reads, 3);
	zassert_equal(gpio_data(port0)->configurations, 1);
}

ZTEST(board_id_gpio, test_unstable_input_failure_is_cached)
{
	gpio_data(port0)->sequence[0] = 0;
	gpio_data(port0)->sequence[1] = BIT(5);
	gpio_data(port0)->sequence_count = 2;
	zassert_equal(device_init(ID(unstable_id)), -EAGAIN);
	zassert_equal(gpio_data(port0)->reads, 2);
	assert_failed_without_resampling(ID(unstable_id), 2);
}

ZTEST(board_id_gpio, test_configuration_error_stops_before_sampling)
{
	gpio_data(port1)->configure_result = -ENOTSUP;
	zassert_equal(device_init(ID(configure_error_id)), -ENOTSUP);
	zassert_equal(gpio_data(port0)->configurations, 1);
	zassert_equal(gpio_data(port1)->configurations, 1);
	zassert_equal(gpio_data(port0)->reads, 0);
	zassert_equal(gpio_data(port1)->reads, 0);
	assert_failed_without_resampling(ID(configure_error_id), 0);
}

ZTEST(board_id_gpio, test_read_error_propagates)
{
	gpio_data(port0)->read_result = -EIO;
	zassert_equal(device_init(ID(read_error_id)), -EIO);
	assert_failed_without_resampling(ID(read_error_id), 1);
}

ZTEST(board_id_gpio, test_unready_gpio_is_rejected)
{
	zassert_equal(device_init(ID(missing_gpio_id)), -ENODEV);
	zassert_equal(event_count, 0);
	assert_failed_without_resampling(ID(missing_gpio_id), 0);
}

ZTEST_SUITE(board_id_gpio, NULL, NULL, before, NULL, NULL);
