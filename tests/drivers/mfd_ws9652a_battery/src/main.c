/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/irq_offload.h>
#include <zephyr/drivers/mfd/ws9652a.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/ws9652a.h>
#include <cwbeat/battery.h>
#include "../../../../drivers/mfd/ws9652a/pmu_context.h"

static int32_t simulated_sleep(int32_t us);
#undef DT_INST_FOREACH_STATUS_OKAY
#define DT_INST_FOREACH_STATUS_OKAY(fn)
int test_pmu_init(const struct device *dev);
#undef DT_DRV_COMPAT
#define k_usleep simulated_sleep
#include "../../../../drivers/adc/adc_ws9652a.c"
#undef k_usleep
#if defined(TEST_BATTERY_HELPER)
#include "../../../../subsys/battery/battery.c"
#endif

enum {
	TRACE_CAPACITY = 512,
	TEST_STACK_SIZE = 1024,
	TEST_PRIORITY = 1
};
enum operation {
	READ,
	WRITE
};
struct operation_record {
	enum operation kind;
	uint8_t index, value;
};
static struct operation_record operations[TRACE_CAPACITY];
static unsigned operation_count, fail_at, raw_count, delay_count, delay_us;
static unsigned prerequisite_polls, battery_polls, prerequisite_ready_at, battery_ready_at;
static uint32_t injected_error;
static uint8_t registers[256];
static uint8_t raw_values[CWBEAT_BATTERY_SAMPLES];
#if defined(TEST_BATTERY_HELPER)
static const struct cwbeat_battery_sample sentinel = {0xa5, {1, 2, 3, 4, 5, 6, 7}};
#endif
static struct ws9652a_pmu_data data;
static struct device_state i2c_state = {.initialized = true};
static struct device_state bridge_state = {.initialized = true};
static const struct device i2c_controller = {.state = &i2c_state};
static const struct device bridge = {.state = &bridge_state};
static const struct ws9652a_pmu_transport transport = {.direct = true};
static const struct ws9652a_pmu_config config = {
	.transport = &transport,
	.bridge = &bridge,
	.i2c = {.bus = &i2c_controller, .addr = 0x34},
};
static struct device_state pmu_state = {.initialized = true};
static const struct device pmu = {
	.name = "test-pmu", .config = &config, .data = &data, .state = &pmu_state,
};
static const struct adc_ws9652a_config adc_config = {.pmu = &pmu};
static struct adc_ws9652a_data adc_data;
static const struct device adc = {
	.name = "test-adc",
	.config = &adc_config,
	.data = &adc_data,
	.api = &adc_ws9652a_api,
};
static bool pause_conversion;
K_SEM_DEFINE(entered, 0, 1);
K_SEM_DEFINE(release, 0, 1);
static int early_status;

static uint32_t record(struct pmu_context *ctx, enum operation kind, uint8_t index, uint8_t value)
{
	zassert_equal_ptr(ctx->config, &config);
	zassert_equal_ptr(ctx->data, &data);
	zassert_true(operation_count < ARRAY_SIZE(operations));
	operations[operation_count++] = (struct operation_record){kind, index, value};
	return operation_count == fail_at ? injected_error : GO4_INTERNAL_BUS_OK;
}

uint32_t go4_internal_bus_read(struct pmu_context *ctx, uint8_t index, uint8_t *out)
{
	uint8_t value = registers[index];
	if (index == PREREQUISITE_STATUS) {
		value = ++prerequisite_polls >= prerequisite_ready_at ? PREREQUISITE_READY : 0;
	} else if (index == BATTERY_STATUS) {
		value = ++battery_polls >= battery_ready_at ? BATTERY_READY : 0;
	} else if (index == BATTERY_RESULT) {
		value = raw_values[raw_count++ % ARRAY_SIZE(raw_values)];
	}
	/* Deliberately populate even on error: stale/partial bytes aren't success. */
	*out = value;
	return record(ctx, READ, index, value);
}

uint32_t go4_internal_bus_verify_backend(struct pmu_context *ctx, uint8_t *out)
{
	uint32_t status = go4_internal_bus_read(ctx, BACKEND_C7, out);
	return status ? status : (*out & 0x0c) == 4 ? 0 : GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND;
}

uint32_t go4_internal_bus_write(struct pmu_context *ctx, uint8_t index, uint8_t value)
{
	/* A reported failure can still have changed the physical register. */
	registers[index] = value;
	if (index == PREREQUISITE_CONTROL && value == TRIGGER_START) {
		prerequisite_polls = 0;
	} else if (index == BATTERY_CONTROL && value == TRIGGER_START) {
		battery_polls = 0;
	}
	return record(ctx, WRITE, index, value);
}

uint32_t pmu_transport_control(const struct pmu_context *ctx)
{
	zassert_equal_ptr(ctx->config, &config);
	zassert_equal_ptr(ctx->data, &data);
	return 0;
}

bool power_policy_start(struct pmu_context *ctx)
{
	ctx->data->preparation_state = POWER_POLICY_DONE;
	ctx->data->backend_ready = true;
	return true;
}

static int32_t simulated_sleep(int32_t us)
{
	zassert_false(k_is_in_isr());
	zassert_true(us == PREREQUISITE_WAIT_US || us == BATTERY_WAIT_US);
	++delay_count;
	delay_us += us;
	if (pause_conversion) {
		pause_conversion = false;
		k_sem_give(&entered);
		zassert_ok(k_sem_take(&release, K_SECONDS(1)));
	}
	return 0;
}

static int before_kernel(void)
{
	uint8_t result;
	early_status = ws9652a_adc_read_raw(&adc, &result, NULL);
	return 0;
}
SYS_INIT(before_kernel, PRE_KERNEL_2, 99);

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	memset(&data, 0, sizeof(data));
	memset(&adc_data, 0, sizeof(adc_data));
	zassert_ok(test_pmu_init(&pmu));
	data.preparation_state = POWER_POLICY_DONE;
	data.backend_ready = true;
	memset(registers, 0, sizeof(registers));
	registers[BACKEND_C7] = 4;
	registers[IDENTITY_F5] = ID_F5_NEEDS_F4;
	registers[IDENTITY_F4] = ID_F4_CLASS_14;
	/* Include unrelated bits in RMW enable registers. */
	registers[PREREQUISITE_ENABLE_08] = 0x80;
	registers[PREREQUISITE_ENABLE_09] = 0x40;
	registers[PREREQUISITE_HIGH] = PREREQUISITE_MINIMUM >> BYTE_BITS;
	registers[PREREQUISITE_LOW] = PREREQUISITE_MINIMUM & UINT8_MAX;
	const uint8_t values[] = {30, 4, 31, 60, 32, 29, 33};
	memcpy(raw_values, values, sizeof(values));
	operation_count = fail_at = raw_count = delay_count = delay_us = 0;
	prerequisite_polls = battery_polls = 0;
	prerequisite_ready_at = battery_ready_at = 1;
	injected_error = GO4_INTERNAL_BUS_STATUS_ERROR;
	pause_conversion = false;
	k_sem_reset(&entered);
	k_sem_reset(&release);
}

ZTEST(pmu_battery, test_context_rejection)
{
	uint8_t result = 0xa5;
	struct ws9652a_adc_diagnostic trace;

	zassert_equal(ws9652a_adc_read_raw(&adc, NULL, &trace), -EINVAL);
	data.preparation_state = POWER_POLICY_IDLE;
	zassert_equal(early_status, -EAGAIN);
	zassert_equal(ws9652a_adc_read_raw(&adc, &result, &trace), -EAGAIN);
	zassert_equal(result, 0xa5);
	zassert_equal(operation_count, 0);
	zassert_equal(trace.cleanup_status, WS9652A_ADC_UNOBSERVED);
}

static void from_isr(const void *unused)
{
	ARG_UNUSED(unused);
	uint8_t result = 0xa5;

	zassert_equal(ws9652a_adc_read_raw(&adc, &result, NULL), -EWOULDBLOCK);
	zassert_equal(result, 0xa5);
}

ZTEST(pmu_battery, test_isr_rejected_without_io)
{
	irq_offload(from_isr, NULL);
	zassert_equal(operation_count, 0);
}

static const struct adc_channel_cfg fixed_channel = {
	.channel_id = WS9652A_ADC_BATTERY_CHANNEL,
	.gain = ADC_GAIN_1,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME_DEFAULT,
};

static struct adc_sequence raw_sequence(uint16_t *buffer)
{
	return (struct adc_sequence){
		.channels = BIT(WS9652A_ADC_BATTERY_CHANNEL),
		.resolution = WS9652A_ADC_RAW_BITS,
		.buffer = buffer,
		.buffer_size = sizeof(*buffer),
	};
}

ZTEST(pmu_battery, test_raw_and_standard_adc_preserve_whole_byte_and_same_transfers)
{
	uint8_t raw = 0;
	uint16_t sample = 0xa5a5;
	struct adc_sequence sequence = raw_sequence(&sample);
	struct ws9652a_adc_diagnostic trace;
	static struct operation_record expected[TRACE_CAPACITY];

	raw_values[0] = 0xd7;
	zassert_ok(ws9652a_adc_read_raw(&adc, &raw, &trace));
	zassert_equal(raw, 0xd7);
	zassert_equal(raw_count, 1);
	zassert_false(trace.blocked);
	unsigned count = operation_count;

	memcpy(expected, operations, count * sizeof(expected[0]));
	reset(NULL);
	raw_values[0] = 0xd7;
	zassert_ok(adc_channel_setup(&adc, &fixed_channel));
	zassert_equal(operation_count, 0);
	zassert_ok(adc_read(&adc, &sequence));
	zassert_equal(sample, 0xd7);
	zassert_equal(operation_count, count);
	for (unsigned i = 0; i < count; ++i) {
		zassert_equal(operations[i].kind, expected[i].kind);
		zassert_equal(operations[i].index, expected[i].index);
		zassert_equal(operations[i].value, expected[i].value);
	}
	zassert_equal(raw_count, 1);
}

ZTEST(pmu_battery, test_adc_channel_features_are_validated_without_io)
{
	struct adc_channel_cfg channel = fixed_channel;

	zassert_equal(adc_channel_setup(&adc, NULL), -EINVAL);
	channel.channel_id = 1;
	zassert_equal(adc_channel_setup(&adc, &channel), -EINVAL);
	channel = fixed_channel;
	channel.differential = true;
	zassert_equal(adc_channel_setup(&adc, &channel), -ENOTSUP);
	channel = fixed_channel;
	channel.gain = ADC_GAIN_1_2;
	zassert_equal(adc_channel_setup(&adc, &channel), -ENOTSUP);
	channel = fixed_channel;
	channel.reference = ADC_REF_VDD_1;
	zassert_equal(adc_channel_setup(&adc, &channel), -ENOTSUP);
	channel = fixed_channel;
	channel.acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 1);
	zassert_equal(adc_channel_setup(&adc, &channel), -ENOTSUP);
#ifdef CONFIG_ADC_CONFIGURABLE_INPUTS
	channel = fixed_channel;
	channel.input_positive = 1;
	zassert_equal(adc_channel_setup(&adc, &channel), -ENOTSUP);
#endif
	zassert_equal(operation_count, 0);
}

ZTEST(pmu_battery, test_adc_sequence_features_and_buffer_are_validated_without_io)
{
	uint16_t sample = 0xa5a5;
	struct adc_sequence sequence = raw_sequence(&sample);
	struct adc_sequence_options options = {0};

	zassert_equal(adc_read(&adc, NULL), -EINVAL);
	sequence.channels = 0;
	zassert_equal(adc_read(&adc, &sequence), -EINVAL);
	sequence.channels = BIT(0) | BIT(1);
	zassert_equal(adc_read(&adc, &sequence), -EINVAL);
	sequence = raw_sequence(&sample);
	sequence.buffer = NULL;
	zassert_equal(adc_read(&adc, &sequence), -EINVAL);
	sequence = raw_sequence(&sample);
	sequence.buffer_size = 1;
	zassert_equal(adc_read(&adc, &sequence), -ENOMEM);
	sequence = raw_sequence(&sample);
	sequence.resolution = 6;
	zassert_equal(adc_read(&adc, &sequence), -ENOTSUP);
	sequence = raw_sequence(&sample);
	sequence.oversampling = 1;
	zassert_equal(adc_read(&adc, &sequence), -ENOTSUP);
	sequence = raw_sequence(&sample);
	sequence.calibrate = true;
	zassert_equal(adc_read(&adc, &sequence), -ENOTSUP);
	sequence = raw_sequence(&sample);
	sequence.options = &options;
	zassert_equal(adc_read(&adc, &sequence), -ENOTSUP);
	sequence = raw_sequence(&sample);
	zassert_equal(adc_read_async(&adc, &sequence, NULL), -ENOTSUP);
	zassert_equal(sample, 0xa5a5);
	zassert_equal(operation_count, 0);
}

ZTEST(pmu_battery, test_adc_reference_voltage_remains_unknown)
{
	uint16_t millivolts = 0xa5a5;

	zassert_equal(adc_ref_get(&adc, ADC_REF_INTERNAL, &millivolts), -ENODATA);
	zassert_equal(adc_ref_get(&adc, ADC_REF_VDD_1, &millivolts), -ENOTSUP);
	zassert_equal(millivolts, 0xa5a5);
	zassert_equal(operation_count, 0);
}

ZTEST(pmu_battery, test_adc_failure_preserves_output_and_child_latches_failed_write)
{
	uint16_t sample = 0xa5a5;
	struct adc_sequence sequence = raw_sequence(&sample);
	uint8_t raw = 0xa5;
	struct ws9652a_adc_diagnostic trace;

	/* C7/F5/F4/08 reads, then the first prerequisite enable write. */
	fail_at = 5;
	zassert_equal(adc_read(&adc, &sequence), -EIO);
	zassert_equal(sample, 0xa5a5);
	zassert_true(adc_data.failure.blocked);
	unsigned before = operation_count;

	zassert_equal(ws9652a_adc_read_raw(&adc, &raw, &trace), -EIO);
	zassert_equal(raw, 0xa5);
	zassert_true(trace.blocked);
	zassert_equal(trace.index, PREREQUISITE_ENABLE_08);
	zassert_equal(trace.bus_status, injected_error);
	zassert_equal(operation_count, before);
}

ZTEST(pmu_battery, test_adc_init_only_checks_parent_readiness)
{
	zassert_ok(adc_ws9652a_init(&adc));
	pmu_state.initialized = false;
	zassert_equal(adc_ws9652a_init(&adc), -ENODEV);
	pmu_state.initialized = true;
	zassert_equal(operation_count, 0);
}

#if defined(TEST_BATTERY_HELPER)
ZTEST(pmu_battery, test_trimmed_mean_and_persistent_enable_bits)
{
	struct cwbeat_battery_sample result;
	struct cwbeat_battery_diagnostic trace;
	zassert_ok(cwbeat_battery_read(&adc, &result, &trace));
	zassert_equal(result.adc, 31);
	zassert_mem_equal(result.raw, raw_values, sizeof(raw_values));
	zassert_equal(trace.completed, CWBEAT_BATTERY_SAMPLES);
	zassert_equal(trace.conversion.prerequisite, PREREQUISITE_MINIMUM);
	zassert_equal(registers[PREREQUISITE_ENABLE_08], 0xa0);
	zassert_equal(registers[PREREQUISITE_ENABLE_09], 0x41);
	zassert_equal(registers[PREREQUISITE_CONTROL], 0);
	zassert_equal(registers[BATTERY_CONTROL], 0);
	zassert_equal(delay_count, 14);
	zassert_equal(delay_us, 6300);
	zassert_false(trace.conversion.blocked);
}

ZTEST(pmu_battery, test_mask_applied_after_trimming_and_average)
{
	const uint8_t values[] = {0, 64, 65, 66, 67, 68, 255};
	memcpy(raw_values, values, sizeof(values));
	struct cwbeat_battery_sample result;
	zassert_ok(cwbeat_battery_read(&adc, &result, NULL));
	zassert_equal(result.adc, 2);
}

ZTEST(pmu_battery, test_failure_at_every_bus_operation_discards_entire_batch)
{
	struct cwbeat_battery_sample result;
	struct cwbeat_battery_diagnostic trace;
	zassert_ok(cwbeat_battery_read(&adc, &result, &trace));
	unsigned normal_count = operation_count;
	bool failed_write[TRACE_CAPACITY];
	for (unsigned i = 0; i < normal_count; ++i) {
		failed_write[i] = operations[i].kind == WRITE;
	}
	for (unsigned failure = 1; failure <= normal_count; ++failure) {
		reset(NULL);
		fail_at = failure;
		result = sentinel;
		zassert_equal(cwbeat_battery_read(&adc, &result, &trace), -EIO, "operation %u",
			      failure);
		zassert_mem_equal(&result, &sentinel, sizeof(result));
		zassert_equal(trace.conversion.bus_status, injected_error);
		zassert_equal(trace.conversion.blocked, failed_write[failure - 1]);
		zassert_equal(registers[PREREQUISITE_CONTROL], 0);
		zassert_equal(registers[BATTERY_CONTROL], 0);
		zassert_true(operation_count <= failure + 1, "only one disarm after failure");
		if (trace.conversion.blocked) {
			unsigned before = operation_count;
			struct cwbeat_battery_diagnostic saved = trace;
			zassert_equal(cwbeat_battery_read(&adc, &result, &trace), -EIO);
			zassert_equal(operation_count, before);
			zassert_mem_equal(&trace.conversion, &saved.conversion,
					  sizeof(trace.conversion));
			zassert_equal(trace.completed, 0);
		}
	}
}

ZTEST(pmu_battery, test_busy_startup_trigger_is_disarmed_without_retry)
{
	registers[BATTERY_CONTROL] = TRIGGER_START;
	struct cwbeat_battery_sample result = sentinel;
	struct cwbeat_battery_diagnostic trace;
	zassert_equal(cwbeat_battery_read(&adc, &result, &trace), -EBUSY);
	zassert_mem_equal(&result, &sentinel, sizeof(result));
	zassert_equal(trace.completed, 0);
	zassert_equal(trace.conversion.index, BATTERY_CONTROL);
	zassert_equal(trace.conversion.value, TRIGGER_START);
	zassert_equal(trace.conversion.cleanup_status, 0);
	zassert_false(trace.conversion.blocked);
	zassert_equal(raw_count, 0);
	zassert_equal(registers[BATTERY_CONTROL], 0);
	zassert_ok(cwbeat_battery_read(&adc, &result, &trace));
	zassert_equal(result.adc, 31);
}

ZTEST(pmu_battery, test_low_prerequisite_does_not_arm_battery)
{
	registers[PREREQUISITE_LOW] -= 1;
	struct cwbeat_battery_sample result = sentinel;
	struct cwbeat_battery_diagnostic trace;
	zassert_equal(cwbeat_battery_read(&adc, &result, &trace), -ERANGE);
	zassert_equal(trace.conversion.prerequisite, PREREQUISITE_MINIMUM - 1);
	zassert_mem_equal(&result, &sentinel, sizeof(result));
	zassert_equal(registers[PREREQUISITE_CONTROL], 0);
	zassert_equal(registers[BATTERY_CONTROL], 0);
	zassert_equal(raw_count, 0);
	for (unsigned i = 0; i < operation_count; ++i) {
		zassert_not_equal(operations[i].index, BATTERY_CONTROL);
	}
}

ZTEST(pmu_battery, test_poll_limits_and_last_poll_success)
{
	struct cwbeat_battery_sample result = sentinel;
	struct cwbeat_battery_diagnostic trace;
	prerequisite_ready_at = PREREQUISITE_POLLS + 1;
	zassert_equal(cwbeat_battery_read(&adc, &result, &trace), -ETIMEDOUT);
	zassert_equal(prerequisite_polls, PREREQUISITE_POLLS);
	zassert_equal(delay_us, 3000);
	zassert_equal(registers[PREREQUISITE_CONTROL], 0);
	zassert_equal(raw_count, 0);
	reset(NULL);
	battery_ready_at = BATTERY_POLLS + 1;
	zassert_equal(cwbeat_battery_read(&adc, &result, &trace), -ETIMEDOUT);
	zassert_equal(battery_polls, BATTERY_POLLS);
	zassert_equal(delay_us, 18300);
	zassert_equal(registers[BATTERY_CONTROL], 0);
	zassert_mem_equal(&result, &sentinel, sizeof(result));
	reset(NULL);
	prerequisite_ready_at = PREREQUISITE_POLLS;
	battery_ready_at = BATTERY_POLLS;
	zassert_ok(cwbeat_battery_read(&adc, &result, &trace));
	zassert_equal(result.adc, 31);
	zassert_equal(delay_us, 147000);
}

ZTEST(pmu_battery, test_backend_and_revision_rejection_precede_writes)
{
	for (unsigned mismatch = 0; mismatch < 3; ++mismatch) {
		reset(NULL);
		registers[mismatch == 0   ? BACKEND_C7
			  : mismatch == 1 ? IDENTITY_F5
					  : IDENTITY_F4] = 0;
		struct cwbeat_battery_sample result = sentinel;
		zassert_equal(cwbeat_battery_read(&adc, &result, NULL), -ENOTSUP);
		zassert_mem_equal(&result, &sentinel, sizeof(result));
		for (unsigned i = 0; i < operation_count; ++i) {
			zassert_equal(operations[i].kind, READ);
		}
	}
}

ZTEST(pmu_battery, test_transport_errno_mapping)
{
	const struct {
		uint32_t bus;
		int error;
	} failures[] = {
		{GO4_INTERNAL_BUS_TIMEOUT, -ETIMEDOUT},
		{GO4_INTERNAL_BUS_BUSY, -EBUSY},
		{GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND, -ENOTSUP},
		{0x410, -ETIMEDOUT},
		{0x412, -EAGAIN},
		{0x413, -ENXIO},
		{0x414, -ENXIO},
	};
	for (unsigned i = 0; i < ARRAY_SIZE(failures); ++i) {
		reset(NULL);
		fail_at = 1;
		injected_error = failures[i].bus;
		struct cwbeat_battery_sample result = sentinel;
		struct cwbeat_battery_diagnostic trace;
		zassert_equal(cwbeat_battery_read(&adc, &result, &trace), failures[i].error);
		zassert_equal(trace.conversion.bus_status, failures[i].bus);
		zassert_equal(operation_count, 1);
		zassert_mem_equal(&result, &sentinel, sizeof(result));
	}
}

#endif /* TEST_BATTERY_HELPER */

K_THREAD_STACK_DEFINE(worker_stack, TEST_STACK_SIZE);
static struct k_thread worker;
static int worker_status;
static void battery_worker(void *unused0, void *unused1, void *unused2)
{
	uint8_t result;
	worker_status = ws9652a_adc_read_raw(&adc, &result, NULL);
}

ZTEST(pmu_battery, test_conversion_lock_spans_sleep_and_cleanup)
{
	pause_conversion = true;
	k_thread_create(&worker, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack), battery_worker,
			NULL, NULL, NULL, TEST_PRIORITY, 0, K_NO_WAIT);
	zassert_ok(k_sem_take(&entered, K_SECONDS(1)));
	unsigned before = operation_count;
	struct ws9652a_pmu_identity identity;
	zassert_equal(ws9652a_pmu_read_identity(&pmu, &identity), -EBUSY);
	zassert_false(ws9652a_pmu_prepare(&pmu));
	zassert_equal(operation_count, before);
	k_sem_give(&release);
	zassert_ok(k_thread_join(&worker, K_SECONDS(1)));
	zassert_ok(worker_status);
	zassert_ok(ws9652a_pmu_read_identity(&pmu, &identity));
}

ZTEST_SUITE(pmu_battery, NULL, NULL, reset, NULL, NULL);
