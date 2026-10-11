/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/irq_offload.h>
#include <zephyr/drivers/mfd/ws9652a.h>
#include "../../../../drivers/mfd/ws9652a/pmu_context.h"

#undef DT_INST_FOREACH_STATUS_OKAY
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#include "../../../../drivers/mfd/ws9652a/mfd_ws9652a.c"

enum {
	BACKEND_C7 = 0xc7,
	VALUE_C7 = 4,
	VALUE_F5 = 1,
	VALUE_F4 = 0xbb,
	TRACE_PREPARE = 0xff,
	WORKER_STACK_SIZE = 1024,
	WORKER_PRIORITY = 1
};
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
static const struct device pmu = {.name = "test-pmu", .config = &config, .data = &data};
static const struct ws9652a_pmu_identity sentinel = {0xa5, 0x5a, 0xa5};
static uint8_t trace[16];
static unsigned trace_count;
static uint8_t fail_index;
static uint32_t failure;
static bool pause_verify, pause_prepare, pause_transaction;
K_SEM_DEFINE(entered, 0, 1);
K_SEM_DEFINE(release, 0, 1);
static int early_read;
static bool early_prepare;
static int early_transaction;
static unsigned callback_calls;

static void record(struct pmu_context *ctx, uint8_t index)
{
	zassert_equal_ptr(ctx->config, &config);
	zassert_equal_ptr(ctx->data, &data);
	zassert_true(trace_count < ARRAY_SIZE(trace));
	trace[trace_count++] = index;
}

static void pause_operation(void)
{
	k_sem_give(&entered);
	zassert_ok(k_sem_take(&release, K_SECONDS(1)));
}

uint32_t go4_internal_bus_verify_backend(struct pmu_context *ctx, uint8_t *out)
{
	record(ctx, BACKEND_C7);
	if (pause_verify) {
		pause_verify = false;
		pause_operation();
	}
	*out = VALUE_C7;
	return fail_index == BACKEND_C7 ? failure : GO4_INTERNAL_BUS_OK;
}

uint32_t go4_internal_bus_read(struct pmu_context *ctx, uint8_t index, uint8_t *out)
{
	record(ctx, index);
	zassert_true(index == PMU_ID_F5 || index == PMU_ID_F4);
	/* A failed low-level transfer is allowed to have written its output. */
	*out = index == PMU_ID_F5 ? VALUE_F5 : VALUE_F4;
	return fail_index == index ? failure : GO4_INTERNAL_BUS_OK;
}

uint32_t go4_internal_bus_write(struct pmu_context *ctx, uint8_t index, uint8_t value)
{
	ARG_UNUSED(value);
	record(ctx, index);
	return fail_index == index ? failure : GO4_INTERNAL_BUS_OK;
}

static int transaction_callback(const struct device *dev, void *context)
{
	zassert_equal_ptr(dev, &pmu);
	++callback_calls;
	if (pause_transaction) {
		pause_transaction = false;
		pause_operation();
	}
	return *(int *)context;
}

bool power_policy_start(struct pmu_context *ctx)
{
	record(ctx, TRACE_PREPARE);
	if (pause_prepare) {
		pause_prepare = false;
		pause_operation();
	}
	ctx->data->diagnostic[POWER_POLICY_STATE] = POWER_POLICY_DONE;
	ctx->data->preparation_state = POWER_POLICY_DONE;
	ctx->data->backend_ready = true;
	return true;
}

static int before_kernel(void)
{
	struct ws9652a_pmu_identity sample = sentinel;
	early_read = ws9652a_pmu_read_identity(&pmu, &sample);
	early_prepare = ws9652a_pmu_prepare(&pmu);
	int result = 0;
	early_transaction = ws9652a_pmu_transaction(&pmu, transaction_callback, &result);
	return 0;
}
SYS_INIT(before_kernel, PRE_KERNEL_2, 99);

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	memset(&data, 0, sizeof(data));
	i2c_state.initialized = true;
	i2c_state.init_res = 0;
	bridge_state.initialized = true;
	bridge_state.init_res = 0;
	zassert_ok(init(&pmu));
	trace_count = callback_calls = 0;
	failure = 0;
	fail_index = 0;
	pause_verify = false;
	pause_prepare = false;
	pause_transaction = false;
	k_sem_reset(&entered);
	k_sem_reset(&release);
}

ZTEST(pmu_runtime, test_init_requires_i2c_and_selected_bridge_without_io)
{
	struct device_state *providers[] = {&i2c_state, &bridge_state};
	for (unsigned i = 0; i < ARRAY_SIZE(providers); i++) {
		providers[i]->initialized = false;
		zassert_equal(init(&pmu), -ENODEV);
		providers[i]->initialized = true;
		providers[i]->init_res = EIO;
		zassert_equal(init(&pmu), -ENODEV);
		providers[i]->init_res = 0;
	}
	zassert_ok(init(&pmu));

	/* An I2C-only PMU does not acquire an unrelated direct bridge. */
	const struct ws9652a_pmu_transport i2c_transport = {.direct = false};
	struct ws9652a_pmu_config i2c_config = config;
	struct device i2c_pmu = {.name = "test-i2c-pmu", .config = &i2c_config, .data = &data};
	i2c_config.transport = &i2c_transport;
	i2c_config.bridge = NULL;
	bridge_state.initialized = false;
	zassert_ok(init(&i2c_pmu));
	i2c_state.initialized = false;
	zassert_equal(init(&i2c_pmu), -ENODEV);
	zassert_equal(trace_count, 0);
}

uint32_t pmu_transport_control(const struct pmu_context *ctx)
{
	zassert_equal_ptr(ctx->config, &config);
	zassert_equal_ptr(ctx->data, &data);
	return 0x5a;
}

ZTEST(pmu_runtime, test_bus_snapshot_uses_transport_provider)
{
	zassert_equal(ws9652a_pmu_bus_control(&pmu), 0x5a);
	zassert_equal(trace_count, 0);
}

ZTEST(pmu_runtime, test_context_and_opt_in)
{
	struct ws9652a_pmu_identity sample = sentinel;
	zassert_equal(ws9652a_pmu_read_identity(&pmu, NULL), -EINVAL);
	zassert_false(early_prepare);
	if (!IS_ENABLED(CONFIG_MFD_WS9652A_RUNTIME_READ)) {
		data.diagnostic[POWER_POLICY_STATE] = POWER_POLICY_DONE;
		data.preparation_state = POWER_POLICY_DONE;
		data.backend_ready = true;
		zassert_equal(early_read, -ENOTSUP);
		zassert_equal(ws9652a_pmu_read_identity(&pmu, &sample), -ENOTSUP);
	} else {
		zassert_equal(early_read, -EAGAIN);
		zassert_equal(ws9652a_pmu_read_identity(&pmu, &sample), -EAGAIN);
		data.diagnostic[POWER_POLICY_STATE] = POWER_POLICY_BUS_ERROR;
		zassert_equal(ws9652a_pmu_read_identity(&pmu, &sample), -EAGAIN);
	}
	zassert_mem_equal(&sample, &sentinel, sizeof(sample));
	zassert_equal(trace_count, 0);
}

static void from_isr(const void *unused)
{
	ARG_UNUSED(unused);
	struct ws9652a_pmu_identity sample = sentinel;
	zassert_true(k_is_in_isr());
	zassert_false(ws9652a_pmu_prepare(&pmu));
	int result = 0;
	zassert_equal(ws9652a_pmu_transaction(&pmu, transaction_callback, &result), -EWOULDBLOCK);
	zassert_equal(ws9652a_pmu_read_identity(&pmu, &sample),
		      IS_ENABLED(CONFIG_MFD_WS9652A_RUNTIME_READ) ? -EWOULDBLOCK : -ENOTSUP);
	zassert_mem_equal(&sample, &sentinel, sizeof(sample));
}

ZTEST(pmu_runtime, test_isr_does_not_touch_bus)
{
	data.diagnostic[POWER_POLICY_STATE] = POWER_POLICY_DONE;
	data.preparation_state = POWER_POLICY_DONE;
	data.backend_ready = true;
	irq_offload(from_isr, NULL);
	zassert_equal(trace_count, 0);
}

ZTEST(pmu_runtime, test_transaction_requires_operational_preparation_state)
{
	int result = -ERANGE;
	zassert_equal(early_transaction, -EAGAIN);
	zassert_equal(ws9652a_pmu_transaction(&pmu, NULL, &result), -EINVAL);
	/* A diagnostic snapshot must not authorize peripheral writes. */
	data.diagnostic[POWER_POLICY_STATE] = POWER_POLICY_DONE;
	data.backend_ready = true;
	zassert_equal(ws9652a_pmu_transaction(&pmu, transaction_callback, &result), -EAGAIN);
	zassert_equal(callback_calls, 0);
	data.preparation_state = POWER_POLICY_DONE;
	data.diagnostic[POWER_POLICY_STATE] = POWER_POLICY_BUS_ERROR;
	zassert_equal(ws9652a_pmu_transaction(&pmu, transaction_callback, &result), -ERANGE);
	zassert_equal(callback_calls, 1);
	zassert_equal(trace_count, 0);
}

#if defined(CONFIG_MFD_WS9652A_RUNTIME_READ)
static const struct ws9652a_pmu_identity expected = {VALUE_C7, VALUE_F5, VALUE_F4};
K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, 2, WORKER_STACK_SIZE);
static struct k_thread workers[2];
static int worker_status[2];
static struct ws9652a_pmu_identity worker_sample[2];

ZTEST(pmu_runtime, test_identity_does_not_repeat_policy_or_change_diagnostics)
{
	struct ws9652a_pmu_identity sample = sentinel;
	zassert_true(ws9652a_pmu_prepare(&pmu));
	uint32_t diagnostics[POWER_POLICY_SNAPSHOT_WORDS];
	memcpy(diagnostics, data.diagnostic, sizeof(diagnostics));
	zassert_ok(ws9652a_pmu_read_identity(&pmu, &sample));
	zassert_mem_equal(&sample, &expected, sizeof(sample));
	zassert_mem_equal(data.diagnostic, diagnostics, sizeof(diagnostics));
	const uint8_t sequence[] = {TRACE_PREPARE, BACKEND_C7, PMU_ID_F5, PMU_ID_F4};
	zassert_equal(trace_count, sizeof(sequence));
	zassert_mem_equal(trace, sequence, sizeof(sequence));
}

ZTEST(pmu_runtime, test_identity_available_after_backend_but_failed_profile)
{
	struct ws9652a_pmu_identity sample = sentinel;
	data.backend_ready = true;
	data.preparation_state = POWER_POLICY_BUS_ERROR;
	data.diagnostic[POWER_POLICY_STATE] = POWER_POLICY_BUS_ERROR;
	zassert_ok(ws9652a_pmu_read_identity(&pmu, &sample));
	zassert_mem_equal(&sample, &expected, sizeof(sample));
	int result = 0;
	zassert_equal(ws9652a_pmu_transaction(&pmu, transaction_callback, &result), -EAGAIN);
	zassert_equal(callback_calls, 0);
	zassert_equal(trace_count, 3);
}

ZTEST(pmu_runtime, test_first_error_stops_without_publishing_partial_sample)
{
	const uint8_t indices[] = {BACKEND_C7, PMU_ID_F5, PMU_ID_F4};
	const struct {
		uint32_t bus;
		int error;
	} errors[] = {
		{GO4_INTERNAL_BUS_TIMEOUT, -ETIMEDOUT},
		{GO4_INTERNAL_BUS_BUSY, -EBUSY},
		{GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND, -ENOTSUP},
		{GO4_INTERNAL_BUS_STATUS_ERROR, -EIO},
		{0x410, -ETIMEDOUT},
		{0x412, -EAGAIN},
		{0x413, -ENXIO},
		{0x414, -ENXIO},
	};
	data.diagnostic[POWER_POLICY_STATE] = POWER_POLICY_DONE;
	data.preparation_state = POWER_POLICY_DONE;
	data.backend_ready = true;
	for (unsigned i = 0; i < ARRAY_SIZE(indices); ++i) {
		for (unsigned j = 0; j < ARRAY_SIZE(errors); ++j) {
			struct ws9652a_pmu_identity sample = sentinel;
			trace_count = 0;
			fail_index = indices[i];
			failure = errors[j].bus;
			zassert_equal(ws9652a_pmu_read_identity(&pmu, &sample), errors[j].error);
			zassert_mem_equal(&sample, &sentinel, sizeof(sample));
			zassert_equal(trace_count, i + 1);
			zassert_mem_equal(trace, indices, i + 1);
		}
	}
}

static void read_worker(void *slot, void *unused1, void *unused2)
{
	unsigned i = (uintptr_t)slot;
	worker_status[i] = ws9652a_pmu_read_identity(&pmu, &worker_sample[i]);
}

static void prepare_worker(void *unused0, void *unused1, void *unused2)
{
	worker_status[0] = ws9652a_pmu_prepare(&pmu) ? 0 : -EIO;
}

static void transaction_worker(void *unused0, void *unused1, void *unused2)
{
	int result = 0;
	worker_status[0] = ws9652a_pmu_transaction(&pmu, transaction_callback, &result);
}

static void start_worker(unsigned slot, k_thread_entry_t entry)
{
	worker_sample[slot] = sentinel;
	worker_status[slot] = -EINPROGRESS;
	k_thread_create(&workers[slot], worker_stacks[slot],
			K_THREAD_STACK_SIZEOF(worker_stacks[slot]), entry, (void *)(uintptr_t)slot,
			NULL, NULL, WORKER_PRIORITY, 0, K_NO_WAIT);
}

ZTEST(pmu_runtime, test_complete_read_sequences_do_not_interleave)
{
	data.diagnostic[POWER_POLICY_STATE] = POWER_POLICY_DONE;
	data.preparation_state = POWER_POLICY_DONE;
	data.backend_ready = true;
	pause_verify = true;
	start_worker(0, read_worker);
	zassert_ok(k_sem_take(&entered, K_SECONDS(1)));
	start_worker(1, read_worker);
	k_sleep(K_MSEC(1));
	zassert_equal(trace_count, 1);
	k_sem_give(&release);
	for (unsigned i = 0; i < ARRAY_SIZE(workers); ++i) {
		zassert_ok(k_thread_join(&workers[i], K_SECONDS(1)));
		zassert_ok(worker_status[i]);
		zassert_mem_equal(&worker_sample[i], &expected, sizeof(expected));
	}
	const uint8_t sequence[] = {BACKEND_C7, PMU_ID_F5, PMU_ID_F4,
				    BACKEND_C7, PMU_ID_F5, PMU_ID_F4};
	zassert_equal(trace_count, sizeof(sequence));
	zassert_mem_equal(trace, sequence, sizeof(sequence));
}

ZTEST(pmu_runtime, test_contended_read_and_prepare_are_bounded)
{
	data.diagnostic[POWER_POLICY_STATE] = POWER_POLICY_DONE;
	data.preparation_state = POWER_POLICY_DONE;
	data.backend_ready = true;
	pause_verify = true;
	start_worker(0, read_worker);
	zassert_ok(k_sem_take(&entered, K_SECONDS(1)));
	struct ws9652a_pmu_identity sample = sentinel;
	zassert_equal(ws9652a_pmu_read_identity(&pmu, &sample), -EBUSY);
	zassert_mem_equal(&sample, &sentinel, sizeof(sample));
	zassert_false(ws9652a_pmu_prepare(&pmu));
	int result = 0;
	zassert_equal(ws9652a_pmu_transaction(&pmu, transaction_callback, &result), -EBUSY);
	zassert_equal(callback_calls, 0);
	zassert_equal(trace_count, 1);
	k_sem_give(&release);
	zassert_ok(k_thread_join(&workers[0], K_SECONDS(1)));
	zassert_ok(worker_status[0]);
}

ZTEST(pmu_runtime, test_child_callback_excludes_all_local_pmu_operations)
{
	data.preparation_state = POWER_POLICY_DONE;
	data.backend_ready = true;
	pause_transaction = true;
	start_worker(0, transaction_worker);
	zassert_ok(k_sem_take(&entered, K_SECONDS(1)));
	struct ws9652a_pmu_identity sample = sentinel;
	int result = 0;
	zassert_equal(ws9652a_pmu_read_identity(&pmu, &sample), -EBUSY);
	zassert_false(ws9652a_pmu_prepare(&pmu));
	zassert_equal(ws9652a_pmu_transaction(&pmu, transaction_callback, &result), -EBUSY);
	zassert_equal(callback_calls, 1);
	zassert_equal(trace_count, 0);
	zassert_mem_equal(&sample, &sentinel, sizeof(sample));
	k_sem_give(&release);
	zassert_ok(k_thread_join(&workers[0], K_SECONDS(1)));
	zassert_ok(worker_status[0]);
	zassert_true(ws9652a_pmu_prepare(&pmu));
	zassert_ok(ws9652a_pmu_transaction(&pmu, transaction_callback, &result));
}

ZTEST(pmu_runtime, test_read_waits_for_preparation_to_finish)
{
	pause_prepare = true;
	start_worker(0, prepare_worker);
	zassert_ok(k_sem_take(&entered, K_SECONDS(1)));
	start_worker(1, read_worker);
	k_sleep(K_MSEC(1));
	zassert_equal(trace_count, 1);
	k_sem_give(&release);
	for (unsigned i = 0; i < ARRAY_SIZE(workers); ++i) {
		zassert_ok(k_thread_join(&workers[i], K_SECONDS(1)));
		zassert_ok(worker_status[i]);
	}
	zassert_mem_equal(&worker_sample[1], &expected, sizeof(expected));
	const uint8_t sequence[] = {TRACE_PREPARE, BACKEND_C7, PMU_ID_F5, PMU_ID_F4};
	zassert_equal(trace_count, sizeof(sequence));
	zassert_mem_equal(trace, sequence, sizeof(sequence));
}
#endif

ZTEST_SUITE(pmu_runtime, NULL, NULL, reset, NULL, NULL);
