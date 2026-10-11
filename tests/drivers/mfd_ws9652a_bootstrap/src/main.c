/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/drivers/i2c/ws9652a.h>
#include <zephyr/drivers/misc/ws9652a_bus.h>
#include <zephyr/drivers/misc/ws9652a_pmu_bridge.h>
#include "pmu_context.h"
#include "go4_internal_bootstrap.h"

/* Compile the public wrapper with a synthetic logical PMU device. The adapters,
 * bootstrap and complete power policy are separate, unmodified production TUs. */
#undef DT_INST_FOREACH_STATUS_OKAY
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#include "../../../../drivers/mfd/ws9652a/mfd_ws9652a.c"

enum {
	REG_C7 = 0xc7, REG_F5 = 0xf5, REG_F4 = 0xf4,
	REG_TEST = 0x66, PMU_ADDRESS = 0x34,
	C7_DIRECT = BIT(2), C7_ALTERNATE = BIT(3), C7_UNRELATED = BIT(5),
	TRACE_CAPACITY = 512, SCRIPT_CAPACITY = 16,
};
enum operation { BRIDGE_RESET, BRIDGE_READ, BRIDGE_WRITE, I2C_READ, I2C_WRITE };
struct trace_entry {
	enum operation operation;
	uint8_t index, value;
};
struct i2c_response {
	enum operation operation;
	uint8_t index, message_index;
	int transfer_error, stop_error;
	size_t data_completed;
};
struct bridge_response {
	uint8_t index, value;
	uint32_t status;
};
static uint8_t registers[256];
static struct trace_entry trace[TRACE_CAPACITY];
static unsigned trace_count, generic_calls, native_calls, snapshot_calls;
static struct i2c_response i2c_script[SCRIPT_CAPACITY];
static unsigned i2c_script_count, i2c_script_cursor;
static struct bridge_response bridge_script[SCRIPT_CAPACITY];
static unsigned bridge_script_count, bridge_script_cursor;
static uint32_t bridge_write_status;
static bool bridge_write_applied, peers_held;
static uint32_t delays[SCRIPT_CAPACITY];
static unsigned delay_count, delay_fail_at, domain_depth;
static int snapshot_error;
static struct device_state i2c_state = {.initialized = true};
static struct device_state bridge_state = {.initialized = true};
static const struct device bridge = {.state = &bridge_state};
static const struct device domain;
static struct ws9652a_pmu_config config;
static struct ws9652a_pmu_data data;
static struct pmu_context context = {&config, &data};
static const struct device pmu = {.name = "pmu", .config = &config, .data = &data};

static void record(enum operation operation, uint8_t index, uint8_t value)
{
	zassert_true(trace_count < ARRAY_SIZE(trace));
	trace[trace_count++] = (struct trace_entry){operation, index, value};
}

static unsigned count_operation(enum operation operation, int index)
{
	unsigned count = 0;
	for (unsigned i = 0; i < trace_count; ++i) {
		count += trace[i].operation == operation &&
			 (index < 0 || trace[i].index == index);
	}
	return count;
}

static void script_i2c(enum operation operation, uint8_t index, int error, int stop,
		       size_t completed, uint8_t message)
{
	zassert_true(i2c_script_count < ARRAY_SIZE(i2c_script));
	i2c_script[i2c_script_count++] = (struct i2c_response){
		.operation = operation, .index = index, .message_index = message,
		.transfer_error = error, .stop_error = stop, .data_completed = completed,
	};
}

static void script_bridge_read(uint8_t index, uint8_t value, uint32_t status)
{
	zassert_true(bridge_script_count < ARRAY_SIZE(bridge_script));
	bridge_script[bridge_script_count++] = (struct bridge_response){index, value, status};
}

static int transfer(struct i2c_msg *msgs, uint8_t count, uint16_t address,
		    struct ws9652a_i2c_result *out)
{
	zassert_equal(address, PMU_ADDRESS);
	zassert_true(count == 1 || count == 2);
	bool read = count == 2;
	zassert_equal(msgs[0].len, read ? 1 : 2);
	zassert_equal(msgs[0].flags, read ? 0 : I2C_MSG_STOP);
	if (read) {
		zassert_equal(msgs[1].len, 1);
		zassert_equal(msgs[1].flags, I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP);
	}
	uint8_t index = msgs[0].buf[0];
	uint8_t value = read ? registers[index] : msgs[0].buf[1];
	enum operation operation = read ? I2C_READ : I2C_WRITE;
	record(operation, index, value);
	struct i2c_response response = {.data_completed = 2};
	if (i2c_script_cursor < i2c_script_count) {
		response = i2c_script[i2c_script_cursor++];
		zassert_equal(operation, response.operation, "script operation %u", i2c_script_cursor);
		zassert_equal(index, response.index, "script register %u", i2c_script_cursor);
	}
	if (read) {
		/* Even an unsuccessful controller may dirty its temporary buffer. The
		 * PMU adapter must not publish that byte to the caller on failure. */
		msgs[1].buf[0] = value;
	} else if (response.data_completed == 2) {
		registers[index] = value;
	}
	*out = (struct ws9652a_i2c_result){
		.transfer_error = response.transfer_error, .stop_error = response.stop_error,
		.message_index = response.message_index, .data_completed = response.data_completed,
		.phase = response.transfer_error ? WS9652A_I2C_PHASE_DATA :
			 response.stop_error ? WS9652A_I2C_PHASE_STOP : WS9652A_I2C_PHASE_COMPLETE,
	};
	return response.transfer_error ? response.transfer_error : response.stop_error;
}

static int fake_i2c_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t count,
			     uint16_t address)
{
	zassert_equal_ptr(dev, config.i2c.bus);
	generic_calls++;
	struct ws9652a_i2c_result result;
	return transfer(msgs, count, address, &result);
}
static DEVICE_API(i2c, fake_i2c_api) = {.transfer = fake_i2c_transfer};
static const struct device i2c_controller = {.state = &i2c_state, .api = &fake_i2c_api};

int ws9652a_i2c_transfer_detailed(const struct device *dev, struct i2c_msg *msgs,
				uint8_t count, uint16_t address, struct ws9652a_i2c_result *out)
{
	zassert_equal_ptr(dev, &i2c_controller);
	zassert_true(config.native_i2c);
	native_calls++;
	return transfer(msgs, count, address, out);
}

const struct device *ws9652a_i2c_bus(const struct device *dev)
{
	zassert_equal_ptr(dev, &i2c_controller);
	return &domain;
}

int ws9652a_bus_lock(const struct device *dev, bool require_held)
{
	zassert_equal_ptr(dev, &domain);
	if (require_held && !peers_held) {
		return -EBUSY;
	}
	domain_depth++;
	return 0;
}

void ws9652a_bus_unlock(const struct device *dev)
{
	zassert_equal_ptr(dev, &domain);
	zassert_true(domain_depth > 0);
	domain_depth--;
}

int ws9652a_i2c_delay(const struct device *dev, uint32_t us)
{
	zassert_equal_ptr(dev, &i2c_controller);
	zassert_true(delay_count < ARRAY_SIZE(delays));
	delays[delay_count++] = us;
	return delay_count == delay_fail_at ? -EIO : 0;
}

int ws9652a_i2c_snapshot(const struct device *dev, uint32_t out[8])
{
	zassert_equal_ptr(dev, &i2c_controller);
	snapshot_calls++;
	if (snapshot_error) {
		return snapshot_error;
	}
	for (unsigned i = 0; i < 8; i++) {
		out[i] = 0xa5000000u + i;
	}
	return 0;
}

uint32_t ws9652a_pmu_bridge_read(const struct device *dev, uint8_t index, uint8_t *out)
{
	zassert_equal_ptr(dev, &bridge);
	if (!out) {
		return WS9652A_PMU_BRIDGE_INVALID_OUTPUT;
	}
	struct bridge_response response = {.index = index, .value = registers[index]};
	if (bridge_script_cursor < bridge_script_count) {
		response = bridge_script[bridge_script_cursor++];
		zassert_equal(index, response.index, "bridge register %u", bridge_script_cursor);
	}
	record(BRIDGE_READ, index, response.value);
	if (!response.status) {
		*out = response.value;
	}
	return response.status;
}

uint32_t ws9652a_pmu_bridge_write(const struct device *dev, uint8_t index, uint8_t value)
{
	zassert_equal_ptr(dev, &bridge);
	record(BRIDGE_WRITE, index, value);
	if (bridge_write_applied) {
		registers[index] = value;
	}
	return bridge_write_status;
}

uint32_t ws9652a_pmu_bridge_reset(const struct device *dev)
{
	zassert_equal_ptr(dev, &bridge);
	record(BRIDGE_RESET, 0, 0);
	return peers_held ? WS9652A_PMU_BRIDGE_OK : WS9652A_PMU_BRIDGE_BUSY;
}

uint32_t ws9652a_pmu_bridge_control(const struct device *dev)
{
	zassert_equal_ptr(dev, &bridge);
	return 0x12345678;
}

static void reset_fixture(void *unused)
{
	ARG_UNUSED(unused);
	memset(&data, 0, sizeof(data));
	memset(registers, 0, sizeof(registers));
	registers[REG_C7] = C7_DIRECT;
	registers[REG_F5] = 1;
	registers[REG_F4] = 0xbb;
	registers[0xc0] = 0x0c;
	registers[0xc2] = 0x0c;
	registers[0x09] = 0xf0;
	config = (struct ws9652a_pmu_config){
		.transport = &ws9652a_pmu_bridge_transport, .bridge = &bridge,
		.i2c = {.bus = &i2c_controller, .addr = PMU_ADDRESS},
		.native_i2c = IS_ENABLED(CONFIG_I2C_WS9652A),
	};
	trace_count = generic_calls = native_calls = snapshot_calls = 0;
	i2c_script_count = i2c_script_cursor = 0;
	bridge_script_count = bridge_script_cursor = 0;
	bridge_write_status = 0;
	bridge_write_applied = peers_held = true;
	delay_count = delay_fail_at = domain_depth = 0;
	snapshot_error = 0;
	zassert_ok(init(&pmu));
}

static void assert_scripts_consumed(void)
{
	zassert_equal(i2c_script_cursor, i2c_script_count);
	zassert_equal(bridge_script_cursor, bridge_script_count);
	zassert_equal(domain_depth, 0);
}

static int child_transaction(const struct device *dev, void *unused)
{
	ARG_UNUSED(unused);
	zassert_equal_ptr(dev, &pmu);
	uint8_t value = 0;
	zassert_equal(ws9652a_pmu_write_locked(dev, REG_TEST, 0xa5), 0);
	zassert_equal(ws9652a_pmu_read_locked(dev, REG_TEST, &value), 0);
	zassert_equal(value, 0xa5);
	return 0;
}

ZTEST(pmu_transports, test_full_policy_identity_and_child_access_use_selected_transport)
{
	for (unsigned native = 0; native <= IS_ENABLED(CONFIG_I2C_WS9652A); native++) {
		for (unsigned direct = 0; direct < 2; direct++) {
			reset_fixture(NULL);
			config.native_i2c = native;
			config.transport = direct ? &ws9652a_pmu_bridge_transport :
				&ws9652a_pmu_i2c_transport;
			config.bridge = direct ? &bridge : NULL;
			registers[REG_C7] = direct ? C7_DIRECT : C7_ALTERNATE;
			zassert_true(ws9652a_pmu_prepare(&pmu));
			zassert_equal(data.preparation_state, POWER_POLICY_DONE);
			zassert_true(data.backend_ready);
			zassert_equal(registers[0x05], 1);
			zassert_equal(registers[0x1b], 1);
			unsigned prepared_count = trace_count;
			zassert_true(ws9652a_pmu_prepare(&pmu));
			zassert_equal(trace_count, prepared_count);
			struct ws9652a_pmu_identity identity = {0};
			zassert_ok(ws9652a_pmu_read_identity(&pmu, &identity));
			zassert_equal(identity.backend_c7, registers[REG_C7]);
			zassert_equal(identity.id_f5, 1);
			zassert_equal(identity.id_f4, 0xbb);
			zassert_ok(ws9652a_pmu_transaction(&pmu, child_transaction, NULL));
			zassert_equal(count_operation(direct ? I2C_READ : BRIDGE_READ, -1), 0);
			zassert_equal(count_operation(direct ? I2C_WRITE : BRIDGE_WRITE, -1), 0);
			zassert_true(count_operation(direct ? BRIDGE_WRITE : I2C_WRITE, -1) > 10);
			zassert_equal(native ? generic_calls : native_calls, 0);
			assert_scripts_consumed();
		}
	}
}

ZTEST(pmu_transports, test_direct_fast_path_requires_bit2_set_and_bit3_clear)
{
	const uint8_t selected[] = {C7_DIRECT, C7_DIRECT | C7_ALTERNATE};
	for (unsigned i = 0; i < ARRAY_SIZE(selected); i++) {
		reset_fixture(NULL);
		registers[REG_C7] = selected[i];
		uint8_t value = 0xa5;
		zassert_equal(go4_internal_bus_init(&context, &value), 0);
		zassert_equal(value, C7_DIRECT);
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_STATE],
			      i == 0 ? GO4_BUS_BOOTSTRAP_FAST : GO4_BUS_BOOTSTRAP_DONE);
		zassert_equal(trace[0].operation, BRIDGE_RESET);
		zassert_equal(trace[1].operation, BRIDGE_READ);
		zassert_equal(count_operation(I2C_READ, REG_C7), i);
		zassert_equal(count_operation(I2C_WRITE, REG_C7), i);
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS], i);
	}
}

ZTEST(pmu_transports, test_initial_read_timeout_allows_only_read_recovery)
{
	for (unsigned native = 0; native <= IS_ENABLED(CONFIG_I2C_WS9652A); native++) {
		reset_fixture(NULL);
		config.native_i2c = native;
		script_bridge_read(REG_C7, 0, GO4_INTERNAL_BUS_STATUS_ERROR);
		for (unsigned i = 0; i < 5; i++) {
			script_i2c(I2C_READ, REG_C7, -ETIMEDOUT, 0, 0, 0);
		}
		zassert_true(ws9652a_pmu_prepare(&pmu));
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_STATE], GO4_BUS_BOOTSTRAP_FAILED);
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_READ_STATUS], GO4_BOOTSTRAP_TRANSFER_TIMEOUT);
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_READ_ATTEMPTS], 5);
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS], 0);
		zassert_equal(data.diagnostic[POWER_POLICY_INIT_STATUS], GO4_INTERNAL_BUS_STATUS_ERROR);
		zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_STATUS], 0);
		zassert_equal(count_operation(I2C_WRITE, REG_C7), 0);
		zassert_equal(count_operation(BRIDGE_WRITE, REG_C7), 0);
		zassert_equal(count_operation(BRIDGE_READ, REG_C7), 2);
		assert_scripts_consumed();
	}
}

ZTEST(pmu_transports, test_i2c_handover_confirms_even_when_bridge_write_reports_error)
{
	for (unsigned native = 0; native <= IS_ENABLED(CONFIG_I2C_WS9652A); native++) {
		reset_fixture(NULL);
		config.native_i2c = native;
		config.transport = &ws9652a_pmu_i2c_transport;
		registers[REG_C7] = C7_DIRECT | C7_ALTERNATE | C7_UNRELATED;
		bridge_write_status = GO4_INTERNAL_BUS_STATUS_ERROR;
		zassert_true(ws9652a_pmu_prepare(&pmu));
		zassert_equal(registers[REG_C7], C7_ALTERNATE | C7_UNRELATED);
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_WRITE_STATUS], bridge_write_status);
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS], 1);
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_ATTEMPTS], 1);
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_STATUS], 0);
		zassert_equal(count_operation(BRIDGE_READ, -1), 1);
		zassert_equal(count_operation(BRIDGE_WRITE, -1), 1);
		zassert_equal(count_operation(BRIDGE_RESET, -1), 0);
		zassert_equal(trace[0].operation, I2C_READ);
		zassert_equal(trace[1].operation, BRIDGE_READ);
		zassert_equal(trace[2].operation, BRIDGE_WRITE);
		zassert_equal(trace[3].operation, I2C_READ);
		zassert_equal(trace[3].index, REG_C7);
		assert_scripts_consumed();
	}
}

ZTEST(pmu_transports, test_i2c_handover_failure_preserves_first_write_error)
{
	config.transport = &ws9652a_pmu_i2c_transport;
	bridge_write_status = GO4_INTERNAL_BUS_TIMEOUT;
	bridge_write_applied = false;
	uint8_t value = 0xa5;
	zassert_equal(go4_internal_bus_init(&context, &value), GO4_INTERNAL_BUS_TIMEOUT);
	zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_WRITE_STATUS], GO4_INTERNAL_BUS_TIMEOUT);
	zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_STATUS],
		      GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND);
	zassert_equal(count_operation(BRIDGE_WRITE, REG_C7), 1);
	zassert_equal(count_operation(I2C_READ, REG_C7), 2);
}

ZTEST(pmu_transports, test_i2c_without_bridge_rejects_inherited_direct_mode)
{
	config.transport = &ws9652a_pmu_i2c_transport;
	config.bridge = NULL;
	zassert_false(ws9652a_pmu_prepare(&pmu));
	zassert_equal(data.diagnostic[POWER_POLICY_INIT_STATUS], GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND);
	zassert_equal(data.diagnostic[POWER_POLICY_BACKEND_C7], C7_DIRECT);
	zassert_equal(data.preparation_state, POWER_POLICY_BUS_ERROR);
	zassert_false(data.backend_ready);
	zassert_equal(trace_count, 1);
	zassert_equal(trace[0].operation, I2C_READ);
}

ZTEST(pmu_transports, test_selected_backend_verification_rejects_wrong_mode_without_writes)
{
	for (unsigned direct = 0; direct < 2; direct++) {
		reset_fixture(NULL);
		config.transport = direct ? &ws9652a_pmu_bridge_transport : &ws9652a_pmu_i2c_transport;
		config.bridge = direct ? &bridge : NULL;
		registers[REG_C7] = direct ? C7_DIRECT | C7_ALTERNATE : C7_DIRECT;
		uint8_t value = 0xa5;
		zassert_equal(go4_internal_bus_verify_backend(&context, &value),
			      GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND);
		zassert_equal(value, registers[REG_C7]);
		zassert_equal(trace_count, 1);
		zassert_equal(trace[0].operation, direct ? BRIDGE_READ : I2C_READ);
	}
}

ZTEST(pmu_transports, test_direct_confirmation_rejects_bit3_even_with_bit2_set)
{
	registers[REG_C7] = 0;
	script_bridge_read(REG_C7, 0, 0);
	script_bridge_read(REG_C7, C7_DIRECT | C7_ALTERNATE, 0);
	uint8_t value = 0xa5;
	zassert_equal(go4_internal_bus_init(&context, &value), GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND);
	zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_STATE], GO4_BUS_BOOTSTRAP_FAILED);
	zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_ATTEMPTS], 1);
	zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_AFTER_C7], C7_DIRECT | C7_ALTERNATE);
	zassert_equal(count_operation(I2C_WRITE, REG_C7), 1);
	assert_scripts_consumed();
}

ZTEST(pmu_transports, test_i2c_error_mapping_stages_read_output)
{
	const struct { int error; uint32_t expected; } cases[] = {
		{-ETIMEDOUT, GO4_BOOTSTRAP_TRANSFER_TIMEOUT},
		{-EAGAIN, GO4_BOOTSTRAP_ARBITRATION},
		{-ENXIO, GO4_BOOTSTRAP_WRITE_NACK},
		{-EBUSY, GO4_INTERNAL_BUS_BUSY},
		{-EIO, GO4_BOOTSTRAP_CLOCK},
	};
	for (unsigned native = 0; native <= IS_ENABLED(CONFIG_I2C_WS9652A); native++) {
		for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
			reset_fixture(NULL);
			config.native_i2c = native;
			config.transport = &ws9652a_pmu_i2c_transport;
			registers[REG_TEST] = 0x5a;
			script_i2c(I2C_READ, REG_TEST, cases[i].error, -ETIMEDOUT, 1, 0);
			uint8_t value = 0xa5;
			zassert_equal(go4_internal_bus_read(&context, REG_TEST, &value), cases[i].expected);
			zassert_equal(value, 0xa5);
			zassert_equal(trace_count, 1);
			assert_scripts_consumed();
		}
	}
}

ZTEST(pmu_transports, test_generic_failed_write_is_never_replayed)
{
	config.native_i2c = false;
	script_i2c(I2C_WRITE, REG_C7, -ETIMEDOUT, 0, 2, 0);
	uint32_t attempts = 0;
	zassert_equal(go4_internal_bootstrap_write(&context, REG_C7, C7_DIRECT, &attempts),
		      GO4_BOOTSTRAP_TRANSFER_TIMEOUT);
	zassert_equal(attempts, 1);
	zassert_equal(generic_calls, 1);
	zassert_equal(registers[REG_C7], C7_DIRECT);
	assert_scripts_consumed();
}

ZTEST(pmu_transports, test_null_reads_do_not_touch_either_transport)
{
	for (unsigned direct = 0; direct < 2; direct++) {
		config.transport = direct ? &ws9652a_pmu_bridge_transport : &ws9652a_pmu_i2c_transport;
		zassert_equal(go4_internal_bus_read(&context, REG_TEST, NULL), GO4_INTERNAL_BUS_INVALID_OUTPUT);
		zassert_equal(go4_internal_bus_verify_backend(&context, NULL), GO4_INTERNAL_BUS_INVALID_OUTPUT);
		zassert_equal(go4_internal_bus_init(&context, NULL), GO4_INTERNAL_BUS_INVALID_OUTPUT);
	}
	zassert_equal(trace_count, 0);
	zassert_equal(snapshot_calls, 0);
}

#if defined(CONFIG_I2C_WS9652A)
ZTEST(pmu_transports, test_acknowledged_i2c_write_stop_failure_uses_read_only_confirmation)
{
	registers[REG_C7] = C7_ALTERNATE | C7_UNRELATED;
	script_i2c(I2C_READ, REG_C7, 0, 0, 2, 0);
	script_i2c(I2C_WRITE, REG_C7, 0, -ETIMEDOUT, 2, 0);
	script_bridge_read(REG_C7, registers[REG_C7], 0);
	script_bridge_read(REG_C7, 0, GO4_INTERNAL_BUS_STATUS_ERROR);
	script_bridge_read(REG_C7, 0, GO4_INTERNAL_BUS_STATUS_ERROR);
	script_bridge_read(REG_C7, C7_DIRECT | C7_UNRELATED, 0);
	zassert_true(ws9652a_pmu_prepare(&pmu));
	zassert_equal(registers[REG_C7], C7_DIRECT | C7_UNRELATED);
	zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_WRITE_STATUS], GO4_BOOTSTRAP_STOP_FAILED);
	zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS], 1);
	zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_ATTEMPTS], 3);
	zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_INITIAL_STATUS], GO4_INTERNAL_BUS_STATUS_ERROR);
	zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_STATUS], 0);
	zassert_equal(count_operation(I2C_WRITE, REG_C7), 1);
	zassert_equal(count_operation(BRIDGE_WRITE, REG_C7), 0);
	zassert_equal(delay_count, 2);
	zassert_equal(delays[0], 160);
	zassert_equal(delays[1], 310);
	assert_scripts_consumed();
}

ZTEST(pmu_transports, test_confirmation_retry_budget_and_delay_failure_never_replay_write)
{
	for (unsigned clock_failure = 0; clock_failure < 2; clock_failure++) {
		reset_fixture(NULL);
		registers[REG_C7] = 0;
		script_i2c(I2C_READ, REG_C7, 0, 0, 2, 0);
		script_i2c(I2C_WRITE, REG_C7, 0, -ETIMEDOUT, 2, 0);
		script_bridge_read(REG_C7, 0, 0);
		unsigned confirmations = clock_failure ? 1 : 6;
		for (unsigned i = 0; i < confirmations; i++) {
			script_bridge_read(REG_C7, 0, 0);
		}
		delay_fail_at = clock_failure ? 1 : 0;
		zassert_false(ws9652a_pmu_prepare(&pmu));
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_ATTEMPTS], confirmations);
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_DELAY_STATUS],
			      clock_failure ? GO4_BOOTSTRAP_CLOCK : GO4_BOOTSTRAP_OK);
		zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_STATUS], POWER_POLICY_UNOBSERVED);
		zassert_equal(count_operation(I2C_WRITE, REG_C7), 1);
		zassert_equal(count_operation(BRIDGE_WRITE, -1), 0);
		zassert_equal(delay_count, clock_failure ? 1 : 5);
		if (!clock_failure) {
			uint32_t sum = 0;
			for (unsigned i = 0; i < delay_count; i++) {
				sum += delays[i];
			}
			zassert_equal(sum, 2300);
		}
		assert_scripts_consumed();
	}
}

ZTEST(pmu_transports, test_native_unacknowledged_write_retries_are_bounded)
{
	for (unsigned i = 0; i < 5; i++) {
		script_i2c(I2C_WRITE, REG_C7, -ENXIO, 0, 0, 0);
	}
	uint32_t attempts = 0;
	registers[REG_C7] = 0;
	zassert_equal(go4_internal_bootstrap_write(&context, REG_C7, C7_DIRECT, &attempts),
		      GO4_BOOTSTRAP_WRITE_NACK);
	zassert_equal(attempts, 5);
	zassert_equal(registers[REG_C7], 0);
	zassert_equal(count_operation(I2C_WRITE, REG_C7), 5);
	zassert_equal(delay_count, 5);
	for (unsigned i = 0; i < delay_count; i++) {
		zassert_equal(delays[i], 150 * (i + 1));
	}
	assert_scripts_consumed();
}

ZTEST(pmu_transports, test_native_value_phase_timeout_or_arbitration_is_not_replayed)
{
	const int errors[] = {-ETIMEDOUT, -EAGAIN};
	const uint32_t statuses[] = {GO4_BOOTSTRAP_TRANSFER_TIMEOUT, GO4_BOOTSTRAP_ARBITRATION};
	for (unsigned i = 0; i < ARRAY_SIZE(errors); i++) {
		reset_fixture(NULL);
		script_i2c(I2C_WRITE, REG_C7, errors[i], -ETIMEDOUT, 1, 0);
		uint32_t attempts = 0;
		zassert_equal(go4_internal_bootstrap_write(&context, REG_C7, C7_DIRECT, &attempts),
			      statuses[i]);
		zassert_equal(attempts, 1);
		zassert_equal(count_operation(I2C_WRITE, REG_C7), 1);
		zassert_equal(delay_count, 0);
		assert_scripts_consumed();
	}
}

ZTEST(pmu_transports, test_native_read_address_and_stop_failures_retain_diagnostics)
{
	uint8_t value = 0xa5;
	script_i2c(I2C_READ, REG_C7, -ENXIO, -ETIMEDOUT, 1, 1);
	zassert_equal(pmu_i2c_read_once(&context, REG_C7, &value), GO4_BOOTSTRAP_READ_ADDRESS);
	zassert_equal(value, 0xa5);
	script_i2c(I2C_READ, REG_C7, 0, -ETIMEDOUT, 2, 1);
	zassert_equal(pmu_i2c_read_once(&context, REG_C7, &value), GO4_BOOTSTRAP_STOP_FAILED);
	zassert_equal(value, 0xa5);
	assert_scripts_consumed();
}

ZTEST(pmu_transports, test_native_preparation_requires_held_peer_admission)
{
	for (unsigned direct = 0; direct < 2; direct++) {
		reset_fixture(NULL);
		config.transport = direct ? &ws9652a_pmu_bridge_transport : &ws9652a_pmu_i2c_transport;
		peers_held = false;
		uint8_t value = 0xa5;
		zassert_equal(go4_internal_bus_init(&context, &value), GO4_INTERNAL_BUS_BUSY);
		zassert_equal(value, 0xa5);
		zassert_equal(data.bootstrap[GO4_BUS_BOOTSTRAP_STATE], GO4_BUS_BOOTSTRAP_FAILED);
		zassert_equal(count_operation(BRIDGE_READ, -1), 0);
		zassert_equal(count_operation(I2C_READ, -1), 0);
		zassert_equal(count_operation(I2C_WRITE, -1), 0);
		zassert_equal(domain_depth, 0);
	}
}
#endif

ZTEST(pmu_transports, test_snapshot_and_control_are_delegated_to_transport)
{
	uint32_t snapshot[8];
	zassert_equal(ws9652a_pmu_bus_control(&pmu), 0x12345678);
	config.transport = &ws9652a_pmu_i2c_transport;
	pmu_transport_snapshot(&context, snapshot);
	for (unsigned i = 0; i < ARRAY_SIZE(snapshot); i++) {
		zassert_equal(snapshot[i], config.native_i2c ? 0xa5000000u + i : UINT32_MAX);
	}
	zassert_equal(ws9652a_pmu_bus_control(&pmu), config.native_i2c ? 0xa5000003u : UINT32_MAX);
	snapshot_error = -EIO;
	pmu_transport_snapshot(&context, snapshot);
	for (unsigned i = 0; i < ARRAY_SIZE(snapshot); i++) {
		zassert_equal(snapshot[i], UINT32_MAX);
	}
	zassert_equal(trace_count, 0);
}

ZTEST_SUITE(pmu_transports, NULL, NULL, reset_fixture, NULL, NULL);
