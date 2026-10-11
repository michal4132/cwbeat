/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/irq_offload.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/misc/ws9652a_bus.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/sys/sys_io.h>

static uint32_t mock_read32(mem_addr_t address);
static void mock_write32(uint32_t value, mem_addr_t address);
static int mock_pinctrl(const struct pinctrl_dev_config *config, uint8_t state);

#undef DT_INST_FOREACH_STATUS_OKAY
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#define sys_read32          mock_read32
#define sys_write32         mock_write32
#define pinctrl_apply_state mock_pinctrl
#include "../../../../drivers/i2c/i2c_ws9652a.c"
#undef pinctrl_apply_state
#undef sys_read32
#undef sys_write32

#define BASE    0x4001c000u
#define GATE    0x424001ecu
#define CORE_ID ((clock_control_subsys_t)1)
#define BUS_ID  ((clock_control_subsys_t)2)

static struct device_state ready = {.initialized = true};
static const struct device domain = {.state = &ready};
static const struct pinctrl_dev_config pins;
static int mock_get_rate(const struct device *dev, clock_control_subsys_t id, uint32_t *rate);
static DEVICE_API(clock_control, clock_api) = {.get_rate = mock_get_rate};
static const struct device clock = {.state = &ready, .api = &clock_api};
static const struct i2c_ws9652a_config config = {
	.base = BASE,
	.clock = &clock,
	.bus = &domain,
	.core_clock = CORE_ID,
	.bus_clock = BUS_ID,
};
static struct i2c_ws9652a_data data;
static const struct device parent = {.state = &ready, .config = &config, .data = &data};
static const struct i2c_ws9652a_channel_config channel_config = {
	.parent = &parent,
	.pinctrl = &pins,
	.route = 0,
};
static const struct i2c_ws9652a_channel_config internal_config = {
	.parent = &parent,
	.route = 1,
};
static struct i2c_ws9652a_channel_data channel_data, internal_data;
static const struct device dev = {
	.config = &channel_config,
	.data = &channel_data,
	.api = &i2c_ws9652a_api,
};
static const struct device internal = {
	.config = &internal_config,
	.data = &internal_data,
	.api = &i2c_ws9652a_api,
};
K_MUTEX_DEFINE(domain_mutex);
static unsigned lock_depth;
static bool peers_held;
static int snapshot_error;
static struct {
	uintptr_t address;
	uint32_t value;
} writes[600];
static unsigned write_count, controller_reads, gate_reads, counter_reads;
static unsigned lock_calls, pin_calls, clock_calls;
static unsigned phase, fail_phase, starts, read_index;
static uint32_t controller[9], gate, core_hz, bus_hz, cycles, counter_control;
static uint32_t initial_status, failed_status, counter_step;
static bool held, stopping, stop_busy, stop_incomplete;
static int lock_error, pin_error, core_error, bus_error;

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	memset(&data, 0, sizeof(data));
	memset(&channel_data, 0, sizeof(channel_data));
	memset(&internal_data, 0, sizeof(internal_data));
	memset(controller, 0, sizeof(controller));
	write_count = controller_reads = gate_reads = counter_reads = 0;
	lock_calls = pin_calls = clock_calls = 0;
	lock_depth = 0;
	peers_held = true;
	snapshot_error = 0;
	phase = fail_phase = starts = read_index = 0;
	gate = cycles = initial_status = failed_status = 0;
	core_hz = bus_hz = 26000000;
	counter_control = WS9652A_DWT_ENABLED;
	counter_step = 10000;
	held = stopping = stop_busy = stop_incomplete = false;
	lock_error = pin_error = core_error = bus_error = 0;
}

static uint32_t mock_read32(mem_addr_t address)
{
	zassert_true(held, "MMIO outside register-bus transaction");
	if (address == WS9652A_DWT_CONTROL) {
		return counter_control;
	}
	if (address == WS9652A_DWT_CYCLES) {
		zassert_true(++counter_reads < 100000, "unbounded counter polling");
		cycles += counter_step;
		return cycles;
	}
	if (address == GATE) {
		++gate_reads;
		return gate;
	}
	zassert_between_inclusive(address, BASE, BASE + 0x20);
	++controller_reads;
	if (address == BASE + REG_STATUS) {
		if (!phase && !stopping) {
			return initial_status;
		}
		if (stopping) {
			return stop_busy         ? STATUS_STOP_BUSY
			       : stop_incomplete ? 0
						 : STATUS_STOP_COMPLETE;
		}
		return phase == fail_phase ? failed_status : 0;
	}
	if (address == BASE + REG_RX) {
		return 0x60 + read_index++;
	}
	return controller[(address - BASE) / 4];
}

static void mock_write32(uint32_t value, mem_addr_t address)
{
	zassert_true(held, "write outside register-bus transaction");
	zassert_true(write_count < ARRAY_SIZE(writes), "unbounded controller writes");
	writes[write_count].address = address;
	writes[write_count++].value = value;
	if (address == GATE) {
		gate = value;
		if (value) {
			phase = 0;
			stopping = false;
		}
		return;
	}
	zassert_between_inclusive(address, BASE, BASE + 0x20);
	controller[(address - BASE) / 4] = value;
	if (address == BASE + REG_COMMAND) {
		if (value & COMMAND_STOP) {
			stopping = true;
		} else {
			++phase;
			if (value == COMMAND_START) {
				++starts;
			}
		}
	}
}

static int mock_pinctrl(const struct pinctrl_dev_config *pinctrl, uint8_t state)
{
	zassert_true(held, "pinctrl outside shared lock");
	zassert_equal_ptr(pinctrl, &pins);
	zassert_equal(state, PINCTRL_STATE_DEFAULT);
	++pin_calls;
	return pin_error;
}

static int mock_get_rate(const struct device *device, clock_control_subsys_t id, uint32_t *rate)
{
	zassert_true(held);
	zassert_equal_ptr(device, &clock);
	zassert_true(id == CORE_ID || id == BUS_ID);
	++clock_calls;
	*rate = id == CORE_ID ? core_hz : bus_hz;
	return id == CORE_ID ? core_error : bus_error;
}

int ws9652a_bus_lock(const struct device *device, bool require_held)
{
	zassert_equal_ptr(device, &domain);
	++lock_calls;
	if (lock_error) {
		return lock_error;
	}
	int error = k_mutex_lock(&domain_mutex, K_MSEC(50));

	if (error) {
		return error;
	}
	if (require_held && !peers_held) {
		k_mutex_unlock(&domain_mutex);
		return -EBUSY;
	}
	++lock_depth;
	held = true;
	return 0;
}

void ws9652a_bus_unlock(const struct device *device)
{
	zassert_equal_ptr(device, &domain);
	zassert_true(lock_depth > 0);
	held = --lock_depth != 0;
	zassert_ok(k_mutex_unlock(&domain_mutex));
}

uint32_t ws9652a_bus_gate_get_locked(const struct device *device)
{
	zassert_equal_ptr(device, &domain);
	return mock_read32(GATE);
}

void ws9652a_bus_gate_set_locked(const struct device *device, uint32_t value)
{
	zassert_equal_ptr(device, &domain);
	mock_write32(value, GATE);
}

int ws9652a_clock_control_get_registers(const struct device *device,
					struct ws9652a_clock_registers *out)
{
	zassert_equal_ptr(device, &clock);
	zassert_true(held);
	if (snapshot_error) {
		return snapshot_error;
	}
	*out = (struct ws9652a_clock_registers){.source = 0x10, .select = 0x209, .divider = 1};
	return 0;
}

static int write_register(void)
{
	uint8_t bytes[] = {1, 0x10};
	struct i2c_msg msg = {.buf = bytes, .len = sizeof(bytes), .flags = I2C_MSG_STOP};

	return i2c_transfer(&dev, &msg, 1, 0x6a);
}

static int read_register(uint8_t *value)
{
	uint8_t index = 9;

	return i2c_write_read(&dev, 0x6a, &index, 1, value, 1);
}

static void assert_cleaned(void)
{
	zassert_equal(gate, 0);
	zassert_equal(controller[REG_CONTROL / 4], 31);
	zassert_equal(controller[REG_ROUTE / 4], 0);
	zassert_true(stopping);
	zassert_false(held);
	zassert_equal(writes[write_count - 2].address, BASE + REG_CONTROL);
	zassert_equal(writes[write_count - 1].address, GATE);
	zassert_equal(writes[write_count - 1].value, 0);
}

ZTEST(i2c_ws9652a, test_init_and_configuration_do_not_touch_hardware)
{
	uint32_t actual = 0;

	zassert_ok(i2c_ws9652a_init(&parent));
	zassert_ok(i2c_ws9652a_channel_init(&dev));
	zassert_ok(i2c_ws9652a_channel_init(&internal));
	zassert_ok(i2c_configure(&dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD)));
	zassert_ok(i2c_get_config(&dev, &actual));
	zassert_equal(actual, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
	zassert_equal(i2c_ws9652a_get_config(&dev, NULL), -EINVAL);
	zassert_equal(i2c_configure(&dev, I2C_SPEED_SET(I2C_SPEED_STANDARD)), -ENOTSUP);
	zassert_equal(i2c_configure(&dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST)),
		      -ENOTSUP);
	zassert_equal(i2c_configure(&dev, actual | I2C_ADDR_10_BITS), -ENOTSUP);
	zassert_equal(write_count + controller_reads + gate_reads + lock_calls + pin_calls, 0);
}

ZTEST(i2c_ws9652a, test_register_write_exact_order_and_no_replay)
{
	zassert_ok(write_register());
	const uintptr_t addresses[] = {GATE,     BASE,     BASE + 0x20, BASE + 8,
				       BASE + 4, BASE + 8, BASE + 4,    BASE + 8,
				       BASE + 4, BASE + 4, BASE,        GATE};
	const uint32_t values[] = {1, BIT(27) | 31, 0, 0xd4, 0x28, 1, 8, 0x10, 8, 0x18, 31, 0};

	zassert_equal(write_count, ARRAY_SIZE(values));
	for (unsigned i = 0; i < write_count; ++i) {
		zassert_equal(writes[i].address, addresses[i]);
		zassert_equal(writes[i].value, values[i]);
	}
	zassert_equal(starts, 1);
	zassert_equal(pin_calls, 1);
	assert_cleaned();
}

ZTEST(i2c_ws9652a, test_register_read_repeated_start_final_nack)
{
	uint8_t value = 0xa5;

	zassert_ok(read_register(&value));
	zassert_equal(value, 0x60);
	zassert_equal(starts, 2);
	const uint32_t commands[] = {0x28, 8, 0x28, 6, 0x16};
	const uint32_t tx[] = {0xd4, 9, 0xd5};
	unsigned c = 0, t = 0;
	for (unsigned i = 0; i < write_count; ++i) {
		if (writes[i].address == BASE + REG_COMMAND) {
			zassert_true(c < ARRAY_SIZE(commands));
			zassert_equal(writes[i].value, commands[c++]);
		}
		if (writes[i].address == BASE + REG_TX) {
			zassert_true(t < ARRAY_SIZE(tx));
			zassert_equal(writes[i].value, tx[t++]);
		}
	}
	zassert_equal(c, ARRAY_SIZE(commands));
	zassert_equal(t, ARRAY_SIZE(tx));
	assert_cleaned();
}

ZTEST(i2c_ws9652a, test_burst_read_ack_intermediate_bytes_and_burst_write_segments)
{
	uint8_t values[3] = {0};

	zassert_ok(i2c_read(&dev, values, sizeof(values), 0x6a));
	const uint8_t expected[] = {0x60, 0x61, 0x62};
	zassert_mem_equal(values, expected, sizeof(values));
	const uint32_t commands[] = {0x28, 4, 4, 6, 0x16};
	unsigned c = 0;
	for (unsigned i = 0; i < write_count; ++i) {
		if (writes[i].address == BASE + REG_COMMAND) {
			zassert_equal(writes[i].value, commands[c++]);
		}
	}
	zassert_equal(c, ARRAY_SIZE(commands));
	assert_cleaned();
	reset(NULL);
	zassert_ok(i2c_burst_write(&dev, 0x6a, 2, expected, sizeof(expected)));
	zassert_equal(starts, 1);
	zassert_equal(phase, 5); /* one address, register, three data bytes */
	assert_cleaned();
}

ZTEST(i2c_ws9652a, test_invalid_patterns_refuse_before_shared_operation)
{
	uint8_t byte = 0;
	struct i2c_msg valid[] = {
		{.buf = &byte, .len = 1, .flags = 0},
		{.buf = &byte, .len = 1, .flags = I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP},
		{.buf = &byte, .len = 1, .flags = I2C_MSG_STOP},
	};
	struct i2c_msg msgs[3];

	zassert_equal(i2c_ws9652a_transfer(&dev, NULL, 1, 0x6a), -EINVAL);
	zassert_equal(i2c_ws9652a_transfer(&dev, valid, 0, 0x6a), -EINVAL);
	zassert_equal(i2c_ws9652a_transfer(&dev, valid, 2, 0x80), -EINVAL);
	zassert_equal(i2c_ws9652a_transfer(&dev, valid, 3, 0x6a), -ENOTSUP);
	for (unsigned variant = 0; variant < 10; ++variant) {
		memcpy(msgs, valid, sizeof(msgs));
		switch (variant) {
		case 0:
			msgs[1].buf = NULL;
			break;
		case 1:
			msgs[1].len = 0;
			break;
		case 2:
			msgs[1].len = MAX_TRANSFER_BYTES;
			break;
		case 3:
			msgs[1].flags |= I2C_MSG_ADDR_10_BITS;
			break;
		case 4:
			msgs[1].flags |= BIT(7);
			break;
		case 5:
			msgs[1].flags &= ~I2C_MSG_STOP;
			break;
		case 6:
			msgs[0].flags |= I2C_MSG_STOP;
			break;
		case 7:
			msgs[0].flags |= I2C_MSG_READ;
			break;
		case 8:
			msgs[1].flags &= ~I2C_MSG_RESTART;
			break;
		case 9:
			msgs[1].flags &= ~I2C_MSG_READ;
			break;
		}
		zassert_true(i2c_ws9652a_transfer(&dev, msgs, 2, 0x6a) < 0);
	}
	zassert_equal(lock_calls + clock_calls + pin_calls, 0);
	zassert_equal(write_count + controller_reads + gate_reads, 0);
}

ZTEST(i2c_ws9652a, test_domain_and_pinctrl_refusal_do_not_start_controller)
{
	peers_held = false;
	zassert_equal(write_register(), -EBUSY);
	zassert_equal(clock_calls + pin_calls + write_count + controller_reads, 0);
	reset(NULL);
	lock_error = -ETIMEDOUT;
	zassert_equal(write_register(), -ETIMEDOUT);
	zassert_equal(pin_calls + write_count, 0);
	reset(NULL);
	pin_error = -EINVAL;
	zassert_equal(write_register(), -EINVAL);
	zassert_equal(write_count, 0);
	zassert_false(channel_data.pins_applied);
}

static void transfer_in_isr(const void *context)
{
	int *result = (int *)context;

	*result = write_register();
}

ZTEST(i2c_ws9652a, test_isr_refuses_without_entering_shared_transaction)
{
	int result = 0;

	irq_offload(transfer_in_isr, &result);
	zassert_equal(result, -EWOULDBLOCK);
	zassert_equal(lock_calls + clock_calls + pin_calls, 0);
	zassert_equal(write_count + controller_reads + gate_reads, 0);
}

ZTEST(i2c_ws9652a, test_clock_and_busy_refusals_precede_pin_writes)
{
	for (unsigned variant = 0; variant < 9; ++variant) {
		reset(NULL);
		int expected = -EIO;
		switch (variant) {
		case 0:
			core_error = -EAGAIN;
			expected = -EAGAIN;
			break;
		case 1:
			bus_error = -ERANGE;
			expected = -ERANGE;
			break;
		case 2:
			core_hz = 0;
			expected = -ERANGE;
			break;
		case 3:
			bus_hz = 0;
			expected = -ERANGE;
			break;
		case 4:
			counter_control = 0;
			break;
		case 5:
			counter_control |= WS9652A_DWT_UNAVAILABLE;
			break;
		case 6:
			initial_status = STATUS_BUSY;
			expected = -EBUSY;
			break;
		case 7:
			initial_status = STATUS_STOP_BUSY;
			expected = -EBUSY;
			break;
		case 8:
			gate = 1;
			expected = -EBUSY;
			break;
		}
		zassert_equal(write_register(), expected);
		zassert_equal(pin_calls + write_count, 0);
		zassert_false(channel_data.pins_applied);
	}
}

ZTEST(i2c_ws9652a, test_nack_and_arbitration_errors_do_not_replay)
{
	for (unsigned failed = 1; failed <= 4; ++failed) {
		for (unsigned arbitration = 0; arbitration < 2; ++arbitration) {
			reset(NULL);
			uint8_t value = 0xa5;
			fail_phase = failed;
			failed_status = arbitration ? STATUS_ARBITRATION : STATUS_NACK;
			/* A read's final NACK is generated by this controller, so the
			 * target-NACK status is only checked through its address phase.
			 */
			int expected = !arbitration && failed == 4 ? 0
				       : arbitration               ? -EAGAIN
								   : -ENXIO;
			zassert_equal(read_register(&value), expected);
			zassert_equal(value, expected ? 0xa5 : 0x60);
			zassert_equal(starts, failed <= 2 ? 1 : 2);
			assert_cleaned();
		}
	}
}

ZTEST(i2c_ws9652a, test_stop_failure_preserves_read_output_and_cleans_controller)
{
	for (unsigned variant = 0; variant < 2; ++variant) {
		reset(NULL);
		stop_busy = variant == 0;
		stop_incomplete = variant == 1;
		uint8_t value = 0xa5;

		zassert_equal(read_register(&value), -ETIMEDOUT);
		zassert_equal(value, 0xa5);
		zassert_equal(starts, 2);
		zassert_true(controller_reads < 520);
		assert_cleaned();
	}
}

ZTEST(i2c_ws9652a, test_stuck_busy_and_stopped_counter_are_bounded)
{
	fail_phase = 1;
	failed_status = STATUS_BUSY;
	zassert_equal(write_register(), -ETIMEDOUT);
	zassert_equal(starts, 1);
	zassert_true(controller_reads < TRANSFER_POLLS + 10);
	assert_cleaned();
	reset(NULL);
	counter_step = 0;
	zassert_equal(write_register(), -EIO);
	zassert_equal(starts, 1);
	zassert_equal(counter_reads, 10 * 26 * 2 + 1);
	assert_cleaned();
}

ZTEST(i2c_ws9652a, test_maximum_length_and_lazy_pinctrl)
{
	uint8_t bytes[MAX_TRANSFER_BYTES] = {0};

	zassert_ok(i2c_read(&dev, bytes, sizeof(bytes), 0x6a));
	zassert_equal(read_index, sizeof(bytes));
	zassert_equal(pin_calls, 1);
	assert_cleaned();
	/* A subsequent call retains pin state, but still rechecks clocks and gate. */
	phase = write_count = controller_reads = gate_reads = clock_calls = 0;
	stopping = false;
	zassert_ok(write_register());
	zassert_equal(pin_calls, 1);
	zassert_equal(lock_calls, 2);
	assert_cleaned();
}

ZTEST(i2c_ws9652a, test_internal_route_without_pinctrl_and_shared_engine)
{
	uint8_t bytes[] = {0xc7, 4};

	zassert_equal_ptr(ws9652a_i2c_bus(&dev), &domain);
	zassert_equal_ptr(ws9652a_i2c_bus(&internal), &domain);
	zassert_ok(i2c_write(&internal, bytes, sizeof(bytes), 0x34));
	zassert_equal(controller[REG_ROUTE / 4], 1);
	zassert_equal(pin_calls, 0);
	zassert_equal(writes[3].value, 0x68);
	zassert_ok(write_register());
	zassert_equal(pin_calls, 1);
	assert_cleaned();
}

ZTEST(i2c_ws9652a, test_detailed_acknowledged_write_and_stop_failure)
{
	uint8_t bytes[] = {0xc7, 4};
	struct i2c_msg msg = {.buf = bytes, .len = sizeof(bytes), .flags = I2C_MSG_STOP};
	struct ws9652a_i2c_result result;

	stop_busy = true;
	zassert_equal(ws9652a_i2c_transfer_detailed(&internal, &msg, 1, 0x34, &result), -ETIMEDOUT);
	zassert_equal(result.transfer_error, 0);
	zassert_equal(result.stop_error, -ETIMEDOUT);
	zassert_equal(result.data_completed, 2);
	zassert_equal(result.phase, WS9652A_I2C_PHASE_STOP);
	zassert_equal(result.message_index, 0);
	zassert_equal(starts, 1);
	zassert_equal(gate, 0);
	zassert_false(held);
}

ZTEST(i2c_ws9652a, test_detailed_write_payload_requires_ack_before_counting)
{
	uint8_t bytes[] = {0xc7, 4};
	struct i2c_msg msg = {.buf = bytes, .len = sizeof(bytes), .flags = I2C_MSG_STOP};
	struct ws9652a_i2c_result result;

	fail_phase = 3; /* Address and index ACKed; value rejected. */
	failed_status = STATUS_NACK;
	zassert_equal(ws9652a_i2c_transfer_detailed(&internal, &msg, 1, 0x34, &result), -ENXIO);
	zassert_equal(result.transfer_error, -ENXIO);
	zassert_equal(result.stop_error, 0);
	zassert_equal(result.phase, WS9652A_I2C_PHASE_DATA);
	zassert_equal(result.data_completed, 1);
	zassert_equal(starts, 1);
	zassert_equal(gate, 0);
	zassert_false(held);
}

ZTEST(i2c_ws9652a, test_detailed_address_and_data_failure_retains_first_error)
{
	uint8_t index = 0xc7, value = 0xaa;
	struct i2c_msg msgs[] = {
		{.buf = &index, .len = 1},
		{.buf = &value, .len = 1, .flags = I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP},
	};
	for (unsigned failed = 1; failed <= 4; ++failed) {
		reset(NULL);
		struct ws9652a_i2c_result result;

		fail_phase = failed;
		failed_status = STATUS_ARBITRATION;
		stop_incomplete = true;
		zassert_equal(ws9652a_i2c_transfer_detailed(&internal, msgs, 2, 0x34, &result),
			      -EAGAIN);
		zassert_equal(result.transfer_error, -EAGAIN);
		zassert_equal(result.stop_error, -ETIMEDOUT);
		zassert_equal(result.phase, failed == 1 || failed == 3 ? WS9652A_I2C_PHASE_ADDRESS
								       : WS9652A_I2C_PHASE_DATA);
		zassert_equal(result.message_index, failed <= 2 ? 0 : 1);
		zassert_equal(result.data_completed, failed <= 2 ? 0 : 1);
		zassert_equal(value, 0xaa);
	}
}

ZTEST(i2c_ws9652a, test_detailed_preflight_and_success)
{
	uint8_t value = 0xaa;
	struct i2c_msg msg = {.buf = &value, .len = 1, .flags = I2C_MSG_READ | I2C_MSG_STOP};
	struct ws9652a_i2c_result result;

	zassert_equal(ws9652a_i2c_transfer_detailed(&internal, NULL, 1, 0x34, &result), -EINVAL);
	zassert_equal(result.transfer_error, -EINVAL);
	zassert_equal(result.phase, WS9652A_I2C_PHASE_PREFLIGHT);
	zassert_equal(result.stop_error + result.data_completed + lock_calls, 0);
	peers_held = false;
	zassert_equal(ws9652a_i2c_transfer_detailed(&internal, &msg, 1, 0x34, &result), -EBUSY);
	zassert_equal(result.transfer_error, -EBUSY);
	zassert_equal(value, 0xaa);
	zassert_equal(write_count, 0);
	peers_held = true;
	zassert_ok(ws9652a_i2c_transfer_detailed(&internal, &msg, 1, 0x34, &result));
	zassert_equal(result.transfer_error + result.stop_error, 0);
	zassert_equal(result.phase, WS9652A_I2C_PHASE_COMPLETE);
	zassert_equal(result.data_completed, 1);
	zassert_equal(value, 0x60);
}

ZTEST(i2c_ws9652a, test_snapshot_staging_and_delay_without_peers_held)
{
	uint32_t snapshot[8];
	uint32_t sentinel[8];

	memset(snapshot, 0xa5, sizeof(snapshot));
	memcpy(sentinel, snapshot, sizeof(snapshot));
	peers_held = false;
	snapshot_error = -EAGAIN;
	zassert_equal(ws9652a_i2c_snapshot(&internal, snapshot), -EAGAIN);
	zassert_mem_equal(snapshot, sentinel, sizeof(snapshot));
	snapshot_error = 0;
	controller[REG_CONTROL / 4] = 31;
	controller[REG_COMMAND / 4] = 0x18;
	controller[REG_ROUTE / 4] = 1;
	gate = 1;
	zassert_ok(ws9652a_i2c_snapshot(&internal, snapshot));
	const uint32_t expected[] = {0x10, 0x209, 1, 31, 0x18, 0, 1, 1};

	zassert_mem_equal(snapshot, expected, sizeof(expected));
	zassert_ok(ws9652a_i2c_delay(&internal, 10));
	zassert_equal(ws9652a_i2c_delay(&internal, UINT32_MAX), -ERANGE);
	counter_step = 0;
	zassert_equal(ws9652a_i2c_delay(&internal, 1), -EIO);
	counter_control = 0;
	zassert_equal(ws9652a_i2c_delay(&internal, 1), -EIO);
	core_hz = 0;
	zassert_equal(ws9652a_i2c_delay(&internal, 1), -ERANGE);
	zassert_equal(write_count, 0);
	zassert_false(held);
}

static void helpers_in_isr(const void *context)
{
	int *result = (int *)context;
	uint32_t snapshot[8];

	result[0] = ws9652a_i2c_delay(&internal, 1);
	result[1] = ws9652a_i2c_snapshot(&internal, snapshot);
}

ZTEST(i2c_ws9652a, test_helpers_require_thread_context)
{
	int results[2] = {0};

	irq_offload(helpers_in_isr, results);
	zassert_equal(results[0], -EWOULDBLOCK);
	zassert_equal(results[1], -EWOULDBLOCK);
	zassert_equal(lock_calls + write_count + controller_reads, 0);
}

K_THREAD_STACK_DEFINE(worker_stack, 1024);
static struct k_thread worker;
K_SEM_DEFINE(worker_started, 0, 1);
static int worker_result;
static uint8_t worker_output;

static void route_worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	k_sem_give(&worker_started);
	worker_result = i2c_read(&internal, &worker_output, 1, 0x34);
}

ZTEST(i2c_ws9652a, test_both_channels_serialize_under_shared_recursive_domain_lock)
{
	uint8_t external_output = 0xaa;

	worker_output = 0xbb;
	zassert_ok(ws9652a_bus_lock(&domain, true));
	k_thread_create(&worker, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack), route_worker,
			NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	zassert_ok(k_sem_take(&worker_started, K_MSEC(20)));
	zassert_equal(write_count, 0);
	zassert_equal(worker_output, 0xbb);
	zassert_ok(i2c_read(&dev, &external_output, 1, 0x6a));
	zassert_equal(external_output, 0x60);
	zassert_equal(worker_output, 0xbb);
	zassert_equal(controller[REG_ROUTE / 4], 0);
	ws9652a_bus_unlock(&domain);
	zassert_ok(k_thread_join(&worker, K_MSEC(100)));
	zassert_ok(worker_result);
	zassert_equal(worker_output, 0x61);
	zassert_equal(controller[REG_ROUTE / 4], 1);
	zassert_equal(pin_calls, 1);
	zassert_equal(gate, 0);
	zassert_false(held);
}

ZTEST_SUITE(i2c_ws9652a, NULL, NULL, reset, NULL, NULL);
