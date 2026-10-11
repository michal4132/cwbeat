/* SPDX-License-Identifier: Apache-2.0 */
#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/mfd/ws9652a.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

BUILD_ASSERT(!IS_ENABLED(CONFIG_I2C_WS9652A));
BUILD_ASSERT(!IS_ENABLED(CONFIG_WS9652A_PMU_BRIDGE));
BUILD_ASSERT(!IS_ENABLED(CONFIG_WS9652A_REGISTER_BUS));
BUILD_ASSERT(!DT_HAS_COMPAT_STATUS_OKAY(zgmicro_ws9652a_pmu_bridge));
BUILD_ASSERT(!DT_HAS_COMPAT_STATUS_OKAY(zgmicro_ws9652a_i2c));

struct bus_data {
	uint8_t registers[256];
	unsigned reads, writes, c7_writes;
	int fail_register;
	bool initialized_post_kernel;
};
static struct bus_data bus_data;

static int bus_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t count,
			uint16_t address)
{
	struct bus_data *data = dev->data;

	if (address != 0x34 || msgs == NULL || count == 0 || msgs[0].buf == NULL ||
	    msgs[0].len == 0) {
		return -EINVAL;
	}
	uint8_t index = msgs[0].buf[0];

	if (count == 2 && msgs[0].len == 1 && msgs[0].flags == 0 && msgs[1].len == 1 &&
	    msgs[1].flags == (I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP)) {
		++data->reads;
		if (data->fail_register == index) {
			return -ENXIO;
		}
		msgs[1].buf[0] = data->registers[index];
		return 0;
	}
	if (count == 1 && msgs[0].len == 2 && msgs[0].flags == I2C_MSG_STOP) {
		++data->writes;
		data->c7_writes += index == 0xc7;
		data->registers[index] = msgs[0].buf[1];
		return 0;
	}
	return -ENOTSUP;
}

static int bus_init(const struct device *dev)
{
	struct bus_data *data = dev->data;

	data->initialized_post_kernel = !k_is_pre_kernel();
	data->fail_register = -1;
	data->registers[0xc7] = 0x80; /* I2C selected, unrelated high bit preserved. */
	data->registers[0xf5] = 1;
	data->registers[0xf4] = 0xbb;
	data->registers[0x09] = 0xff;
	return 0;
}

static DEVICE_API(i2c, bus_api) = {.transfer = bus_transfer};
I2C_DEVICE_DT_DEFINE(DT_NODELABEL(test_i2c), bus_init, NULL, &bus_data, NULL, POST_KERNEL,
		     CONFIG_I2C_INIT_PRIORITY, &bus_api);

static const struct device *const pmu = DEVICE_DT_GET(DT_NODELABEL(test_pmu));
static const struct device *const bus = DEVICE_DT_GET(DT_NODELABEL(test_i2c));
static const struct device *const adc = DEVICE_DT_GET(DT_NODELABEL(test_adc));
static bool early_bus_ready, early_pmu_ready, early_adc_ready;
static bool late_bus_ready, late_pmu_ready, late_adc_ready, initialization_read_only;

static int capture_early(void)
{
	early_bus_ready = device_is_ready(bus);
	early_pmu_ready = device_is_ready(pmu);
	early_adc_ready = device_is_ready(adc);
	return 0;
}
SYS_INIT(capture_early, PRE_KERNEL_2, 99);

static int capture_late(void)
{
	late_bus_ready = device_is_ready(bus);
	late_pmu_ready = device_is_ready(pmu);
	late_adc_ready = device_is_ready(adc);
	initialization_read_only = bus_data.reads == 0 && bus_data.writes == 0;
	return 0;
}
SYS_INIT(capture_late, POST_KERNEL, 99);

static void *setup(void)
{
	struct ws9652a_pmu_identity identity;

	zassert_true(device_is_ready(pmu), "generic I2C PMU initialized before its bus");
	zassert_equal(ws9652a_pmu_read_identity(pmu, &identity), -EAGAIN);
	zassert_equal(bus_data.reads + bus_data.writes, 0);
	zassert_true(ws9652a_pmu_prepare(pmu));
	zassert_true(bus_data.reads > 0 && bus_data.writes > 0);
	zassert_equal(bus_data.c7_writes, 0);
	return NULL;
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	bus_data.fail_register = -1;
	bus_data.registers[0xc7] = 0x80;
}

ZTEST(mfd_ws9652a_i2c_only, test_generic_bus_and_pmu_initialize_post_kernel)
{
	zassert_false(early_bus_ready);
	zassert_false(early_pmu_ready);
	zassert_false(early_adc_ready);
	zassert_true(late_bus_ready);
	zassert_true(late_pmu_ready);
	zassert_true(late_adc_ready);
	zassert_true(bus_data.initialized_post_kernel);
	zassert_true(initialization_read_only);
}

ZTEST(mfd_ws9652a_i2c_only, test_adc_child_ready_and_channel_setup_performs_no_io)
{
	const struct adc_channel_cfg channel = {
		.channel_id = 0,
		.gain = ADC_GAIN_1,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
	};
	unsigned reads = bus_data.reads;
	unsigned writes = bus_data.writes;

	zassert_true(device_is_ready(adc));
	zassert_ok(adc_channel_setup(adc, &channel));
	zassert_equal(bus_data.reads, reads);
	zassert_equal(bus_data.writes, writes);
}

ZTEST(mfd_ws9652a_i2c_only, test_identity_uses_selected_generic_i2c)
{
	struct ws9652a_pmu_identity identity;
	unsigned reads = bus_data.reads;
	unsigned writes = bus_data.writes;

	zassert_ok(ws9652a_pmu_read_identity(pmu, &identity));
	zassert_equal(identity.backend_c7, 0x80);
	zassert_equal(identity.id_f5, 1);
	zassert_equal(identity.id_f4, 0xbb);
	zassert_equal(bus_data.reads - reads, 3);
	zassert_equal(bus_data.writes, writes);
}

ZTEST(mfd_ws9652a_i2c_only, test_failed_identity_does_not_publish_partial_sample)
{
	struct ws9652a_pmu_identity identity = {0xa1, 0xb2, 0xc3};
	const struct ws9652a_pmu_identity expected = identity;
	unsigned reads = bus_data.reads;

	bus_data.fail_register = 0xf4;
	zassert_equal(ws9652a_pmu_read_identity(pmu, &identity), -ENXIO);
	zassert_mem_equal(&identity, &expected, sizeof(identity));
	zassert_equal(bus_data.reads - reads, 3);
}

ZTEST(mfd_ws9652a_i2c_only, test_runtime_mode_mismatch_does_not_write_c7)
{
	struct ws9652a_pmu_identity identity = {0xa1, 0xb2, 0xc3};
	const struct ws9652a_pmu_identity expected = identity;
	unsigned reads = bus_data.reads;
	unsigned writes = bus_data.writes;

	bus_data.registers[0xc7] |= BIT(2);
	zassert_equal(ws9652a_pmu_read_identity(pmu, &identity), -ENOTSUP);
	zassert_mem_equal(&identity, &expected, sizeof(identity));
	zassert_equal(bus_data.reads - reads, 1);
	zassert_equal(bus_data.writes, writes);
}

static int child_operation(const struct device *dev, void *context)
{
	uint8_t *value = context;
	uint32_t status = ws9652a_pmu_write_locked(dev, 0x61, 0xa5);

	if (!status) {
		status = ws9652a_pmu_read_locked(dev, 0x61, value);
	}
	return ws9652a_pmu_bus_error(status);
}

ZTEST(mfd_ws9652a_i2c_only, test_prepared_child_transaction_uses_generic_i2c)
{
	uint8_t value = 0;

	zassert_ok(ws9652a_pmu_transaction(pmu, child_operation, &value));
	zassert_equal(value, 0xa5);
	zassert_equal(bus_data.registers[0x61], 0xa5);
}

ZTEST_SUITE(mfd_ws9652a_i2c_only, NULL, setup, before, NULL, NULL);
