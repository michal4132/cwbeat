/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include "go4_internal_bootstrap.h"
#include "pmu_context.h"

enum operation {
	INIT,
	VERIFY,
	READ,
	WRITE
};
struct transaction {
	enum operation operation;
	uint8_t index, value;
};
static struct transaction transactions[256];
static unsigned count, init_calls, verify_calls, read_calls, write_calls;
static uint8_t registers[256], init_c7, verify_c7, failed_read_index, failed_write_index;
static uint32_t init_status, verify_status, failed_read_status, failed_write_status;
static uint32_t initial_bootstrap[GO4_BUS_BOOTSTRAP_WORDS];
static struct ws9652a_pmu_data data;
static const struct ws9652a_pmu_config config;
static struct pmu_context context = {&config, &data};

static void record(enum operation operation, uint8_t index, uint8_t value)
{
	zassert_true(count < ARRAY_SIZE(transactions));
	transactions[count++] = (struct transaction){operation, index, value};
}

uint32_t go4_internal_bus_init(struct pmu_context *ctx, uint8_t *value)
{
	zassert_equal_ptr(ctx, &context);
	++init_calls;
	record(INIT, 0xc7, init_c7);
	memcpy(ctx->data->bootstrap, initial_bootstrap, sizeof(initial_bootstrap));
	*value = init_c7;
	return init_status;
}

uint32_t go4_internal_bus_verify_backend(struct pmu_context *ctx, uint8_t *value)
{
	zassert_equal_ptr(ctx, &context);
	++verify_calls;
	record(VERIFY, 0xc7, verify_c7);
	*value = verify_c7;
	return verify_status;
}

uint32_t go4_internal_bus_read(struct pmu_context *ctx, uint8_t index, uint8_t *value)
{
	zassert_equal_ptr(ctx, &context);
	++read_calls;
	record(READ, index, registers[index]);
	*value = registers[index];
	return index == failed_read_index ? failed_read_status : GO4_INTERNAL_BUS_OK;
}

uint32_t go4_internal_bus_write(struct pmu_context *ctx, uint8_t index, uint8_t value)
{
	zassert_equal_ptr(ctx, &context);
	++write_calls;
	record(WRITE, index, value);
	registers[index] = value;
	return index == failed_write_index ? failed_write_status : GO4_INTERNAL_BUS_OK;
}

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	memset(&data, 0, sizeof(data));
	memset(registers, 0, sizeof(registers));
	memset(transactions, 0, sizeof(transactions));
	count = init_calls = verify_calls = read_calls = write_calls = 0;
	init_status = verify_status = failed_read_status = failed_write_status =
		GO4_INTERNAL_BUS_OK;
	init_c7 = verify_c7 = 0x04;
	failed_read_index = failed_write_index = 0xff;
	registers[0xf5] = 0x01;
	registers[0xf4] = 0xbb;
	registers[0xc0] = 0x0c; /* Native nonzero-state path, no temporary 04 test. */
	registers[0xc2] = 0x0c;
	registers[0x09] = 0xf0;
	for (unsigned i = 0; i < ARRAY_SIZE(initial_bootstrap); ++i) {
		initial_bootstrap[i] = 0xa5000000u + i;
	}
	initial_bootstrap[GO4_BUS_BOOTSTRAP_STATE] = GO4_BUS_BOOTSTRAP_FAST;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_DIRECT_STATUS] = GO4_INTERNAL_BUS_OK;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_DIRECT_C7] = 0x04;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_READ_STATUS] = UINT32_MAX;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_READ_ATTEMPTS] = 0;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS] = 0;
}

static void failed_alternate_read(void)
{
	init_status = GO4_INTERNAL_BUS_STATUS_ERROR;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_STATE] = GO4_BUS_BOOTSTRAP_FAILED;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_DIRECT_STATUS] = init_status;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_DIRECT_C7] = UINT32_MAX;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_READ_STATUS] = GO4_BOOTSTRAP_TRANSFER_TIMEOUT;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_READ_ATTEMPTS] = 5;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS] = 0;
}

static void bootstrap_is_unchanged(void)
{
	zassert_mem_equal(data.bootstrap, initial_bootstrap, sizeof(initial_bootstrap));
}

static void repeated_call_is_inert(bool result)
{
	struct ws9652a_pmu_data saved = data;
	unsigned previous_count = count;

	zassert_equal(power_policy_start(&context), result);
	zassert_equal(count, previous_count);
	zassert_mem_equal(&data, &saved, sizeof(data));
}

static void identity_precedes_first_write(unsigned first_read)
{
	unsigned first_write = first_read;

	while (first_write < count && transactions[first_write].operation != WRITE) {
		++first_write;
	}
	zassert_true(first_write < count, "Expected normal policy writes");
	zassert_true(first_write > first_read + 1);
	for (unsigned i = first_read; i < first_write; ++i) {
		zassert_equal(transactions[i].operation, READ);
		zassert_equal(transactions[i].index, (i - first_read) % 2 ? 0xf4 : 0xf5);
	}
}

ZTEST(ws9652a_pmu_policy, test_healthy_policy_keeps_native_sequence)
{
	/* Canonical class-14 nonzero-state trace: 84 reads and these 14 writes. */
	static const uint8_t expected[][2] = {
		{0x0c, 0x01}, {0x0d, 0x01}, {0x02, 0x51}, {0x12, 0x01}, {0x00, 0x0c},
		{0x07, 0x02}, {0x01, 0x1a}, {0x09, 0x00}, {0x04, 0x5a}, {0x02, 0x59},
		{0xc1, 0x0c}, {0xc2, 0x04}, {0x05, 0x01}, {0x1b, 0x01},
	};

	zassert_true(power_policy_start(&context));
	zassert_equal(init_calls, 1);
	zassert_equal(verify_calls, 0);
	zassert_equal(read_calls, 84);
	zassert_equal(write_calls, ARRAY_SIZE(expected));
	unsigned n = 0;
	for (unsigned i = 0; i < count; ++i) {
		if (transactions[i].operation == WRITE) {
			zassert_equal(transactions[i].index, expected[n][0]);
			zassert_equal(transactions[i].value, expected[n++][1]);
		}
	}
	identity_precedes_first_write(1);
	zassert_equal(data.diagnostic[POWER_POLICY_STATE], POWER_POLICY_DONE);
	zassert_equal(data.preparation_state, POWER_POLICY_DONE);
	zassert_true(data.backend_ready);
	zassert_equal(data.diagnostic[POWER_POLICY_INIT_STATUS], GO4_INTERNAL_BUS_OK);
	zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_STATUS], POWER_POLICY_UNOBSERVED);
	zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_C7], POWER_POLICY_UNOBSERVED);
	zassert_equal(data.diagnostic[POWER_POLICY_OPERATION_COUNTS], 84u | (14u << 16));
	bootstrap_is_unchanged();
	repeated_call_is_inert(true);
}

ZTEST(ws9652a_pmu_policy, test_recovery_only_prepends_one_verification)
{
	static struct transaction healthy[ARRAY_SIZE(transactions)];

	zassert_true(power_policy_start(&context));
	unsigned healthy_count = count;
	memcpy(healthy, transactions, sizeof(healthy));
	reset(NULL);
	failed_alternate_read();
	zassert_true(power_policy_start(&context));
	zassert_equal(init_calls, 1);
	zassert_equal(verify_calls, 1);
	zassert_equal(transactions[0].operation, INIT);
	zassert_equal(transactions[1].operation, VERIFY);
	zassert_equal(count, healthy_count + 1);
	zassert_mem_equal(&transactions[2], &healthy[1], (healthy_count - 1) * sizeof(healthy[0]));
	identity_precedes_first_write(2);
	zassert_equal(data.diagnostic[POWER_POLICY_INIT_STATUS], GO4_INTERNAL_BUS_STATUS_ERROR);
	zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_STATUS], GO4_INTERNAL_BUS_OK);
	zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_C7], 0x04);
	zassert_equal(data.diagnostic[POWER_POLICY_BACKEND_C7], 0x04);
	zassert_equal(data.diagnostic[POWER_POLICY_OPERATION_COUNTS], 85u | (14u << 16));
	bootstrap_is_unchanged();
	repeated_call_is_inert(true);
}

ZTEST(ws9652a_pmu_policy, test_recheck_errors_stop_before_identity_or_writes)
{
	const uint32_t errors[] = {GO4_INTERNAL_BUS_TIMEOUT, GO4_INTERNAL_BUS_STATUS_ERROR,
				   GO4_INTERNAL_BUS_BUSY};

	for (unsigned i = 0; i < ARRAY_SIZE(errors); ++i) {
		reset(NULL);
		failed_alternate_read();
		verify_status = errors[i];
		zassert_false(power_policy_start(&context));
		zassert_equal(verify_calls, 1);
		zassert_false(data.backend_ready);
		zassert_equal(data.preparation_state, POWER_POLICY_BUS_ERROR);
		zassert_equal(read_calls, 0);
		zassert_equal(write_calls, 0);
		zassert_equal(data.diagnostic[POWER_POLICY_STATE], POWER_POLICY_BUS_ERROR);
		zassert_equal(data.diagnostic[POWER_POLICY_LAST_STEP],
			      POWER_POLICY_STEP_BACKEND_VERIFY);
		zassert_equal(data.diagnostic[POWER_POLICY_INIT_STATUS],
			      GO4_INTERNAL_BUS_STATUS_ERROR);
		zassert_equal(data.diagnostic[POWER_POLICY_BUS_STATUS], errors[i]);
		zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_STATUS], errors[i]);
		zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_C7], POWER_POLICY_UNOBSERVED);
		zassert_equal(data.diagnostic[POWER_POLICY_REGISTER_VALUE],
			      POWER_POLICY_UNOBSERVED);
		zassert_equal(data.diagnostic[POWER_POLICY_OPERATION_COUNTS], 1);
		bootstrap_is_unchanged();
		verify_status = GO4_INTERNAL_BUS_OK;
		repeated_call_is_inert(false);
	}
}

ZTEST(ws9652a_pmu_policy, test_wrong_route_is_retained_without_writes)
{
	failed_alternate_read();
	verify_status = GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND;
	verify_c7 = 0x0c;
	zassert_false(power_policy_start(&context));
	zassert_equal(verify_calls, 1);
	zassert_equal(read_calls, 0);
	zassert_equal(write_calls, 0);
	zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_C7], 0x0c);
	zassert_equal(data.diagnostic[POWER_POLICY_BUS_STATUS],
		      GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND);
	zassert_equal(data.diagnostic[POWER_POLICY_INIT_STATUS], GO4_INTERNAL_BUS_STATUS_ERROR);
	bootstrap_is_unchanged();
	repeated_call_is_inert(false);
}

ZTEST(ws9652a_pmu_policy, test_other_alternate_errors_do_not_recheck)
{
	const uint32_t errors[] = {GO4_BOOTSTRAP_ARBITRATION, GO4_BOOTSTRAP_READ_ADDRESS,
				   GO4_BOOTSTRAP_WRITE_NACK, GO4_BOOTSTRAP_STOP_FAILED,
				   GO4_BOOTSTRAP_CLOCK};

	for (unsigned i = 0; i < ARRAY_SIZE(errors); ++i) {
		reset(NULL);
		failed_alternate_read();
		initial_bootstrap[GO4_BUS_BOOTSTRAP_READ_STATUS] = errors[i];
		zassert_false(power_policy_start(&context));
		zassert_equal(count, 1);
		zassert_equal(verify_calls + read_calls + write_calls, 0);
		zassert_equal(data.diagnostic[POWER_POLICY_BUS_STATUS], init_status);
		zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_STATUS],
			      POWER_POLICY_UNOBSERVED);
		bootstrap_is_unchanged();
		repeated_call_is_inert(false);
	}
}

ZTEST(ws9652a_pmu_policy, test_busy_and_unattempted_fallback_do_not_recheck)
{
	failed_alternate_read();
	init_status = GO4_INTERNAL_BUS_BUSY;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_DIRECT_STATUS] = UINT32_MAX;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_READ_STATUS] = UINT32_MAX;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_READ_ATTEMPTS] = 0;
	zassert_false(power_policy_start(&context));
	zassert_equal(count, 1);
	zassert_equal(data.diagnostic[POWER_POLICY_INIT_STATUS], GO4_INTERNAL_BUS_BUSY);
	bootstrap_is_unchanged();
	reset(NULL);
	failed_alternate_read();
	initial_bootstrap[GO4_BUS_BOOTSTRAP_READ_ATTEMPTS] = 0;
	zassert_false(power_policy_start(&context));
	zassert_equal(count, 1);
	bootstrap_is_unchanged();
}

ZTEST(ws9652a_pmu_policy, test_post_write_failure_never_uses_read_recovery)
{
	failed_alternate_read();
	initial_bootstrap[GO4_BUS_BOOTSTRAP_READ_STATUS] = GO4_BOOTSTRAP_OK;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_WRITE_STATUS] = GO4_BOOTSTRAP_STOP_FAILED;
	initial_bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS] = 1;
	zassert_false(power_policy_start(&context));
	zassert_equal(count, 1);
	bootstrap_is_unchanged();
	/* Even an inconsistent timeout snapshot must not permit a write replay. */
	reset(NULL);
	failed_alternate_read();
	initial_bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS] = 1;
	zassert_false(power_policy_start(&context));
	zassert_equal(count, 1);
	bootstrap_is_unchanged();
}

ZTEST(ws9652a_pmu_policy, test_recovery_requires_failed_bootstrap_state)
{
	const uint32_t states[] = {GO4_BUS_BOOTSTRAP_IDLE, GO4_BUS_BOOTSTRAP_RUNNING,
				   GO4_BUS_BOOTSTRAP_FAST, GO4_BUS_BOOTSTRAP_DONE};

	for (unsigned i = 0; i < ARRAY_SIZE(states); ++i) {
		reset(NULL);
		failed_alternate_read();
		initial_bootstrap[GO4_BUS_BOOTSTRAP_STATE] = states[i];
		zassert_false(power_policy_start(&context));
		zassert_equal(count, 1);
		bootstrap_is_unchanged();
	}
}

ZTEST(ws9652a_pmu_policy, test_recovered_transport_still_requires_supported_identity)
{
	failed_alternate_read();
	registers[0xf4] = 0xba;
	zassert_false(power_policy_start(&context));
	zassert_equal(verify_calls, 1);
	zassert_equal(read_calls, 2);
	zassert_equal(write_calls, 0);
	zassert_equal(transactions[2].index, 0xf5);
	zassert_equal(transactions[3].index, 0xf4);
	zassert_equal(data.diagnostic[POWER_POLICY_STATE], POWER_POLICY_UNSUPPORTED_REVISION);
	zassert_equal(data.preparation_state, POWER_POLICY_UNSUPPORTED_REVISION);
	zassert_true(data.backend_ready);
	zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_STATUS], GO4_INTERNAL_BUS_OK);
	zassert_equal(data.diagnostic[POWER_POLICY_INIT_STATUS], GO4_INTERNAL_BUS_STATUS_ERROR);
	bootstrap_is_unchanged();
	repeated_call_is_inert(false);
}

ZTEST(ws9652a_pmu_policy, test_identity_transport_error_after_recovery_stops_writes)
{
	failed_alternate_read();
	failed_read_index = 0xf5;
	failed_read_status = GO4_INTERNAL_BUS_TIMEOUT;
	zassert_false(power_policy_start(&context));
	zassert_equal(verify_calls, 1);
	zassert_equal(read_calls, 1);
	zassert_equal(write_calls, 0);
	zassert_equal(data.diagnostic[POWER_POLICY_STATE], POWER_POLICY_BUS_ERROR);
	zassert_equal(data.diagnostic[POWER_POLICY_LAST_STEP], POWER_POLICY_STEP_IDENTITY_F5);
	zassert_equal(data.diagnostic[POWER_POLICY_BUS_STATUS], GO4_INTERNAL_BUS_TIMEOUT);
	zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_STATUS], GO4_INTERNAL_BUS_OK);
	bootstrap_is_unchanged();
	repeated_call_is_inert(false);
}

ZTEST(ws9652a_pmu_policy, test_partial_policy_failure_does_not_restart_recovery)
{
	failed_alternate_read();
	failed_write_index = 0x12;
	failed_write_status = GO4_INTERNAL_BUS_TIMEOUT;
	zassert_false(power_policy_start(&context));
	zassert_equal(verify_calls, 1);
	zassert_equal(write_calls, 4);
	zassert_true(data.backend_ready);
	zassert_equal(data.preparation_state, POWER_POLICY_BUS_ERROR);
	zassert_equal(data.diagnostic[POWER_POLICY_STATE], POWER_POLICY_BUS_ERROR);
	zassert_equal(data.diagnostic[POWER_POLICY_LAST_STEP], POWER_POLICY_STEP_FIXED);
	zassert_equal(data.diagnostic[POWER_POLICY_BUS_STATUS], GO4_INTERNAL_BUS_TIMEOUT);
	zassert_equal(data.diagnostic[POWER_POLICY_REGISTER_INDEX], 0x12);
	zassert_equal(data.diagnostic[POWER_POLICY_INIT_STATUS], GO4_INTERNAL_BUS_STATUS_ERROR);
	zassert_equal(data.diagnostic[POWER_POLICY_RECOVERY_STATUS], GO4_INTERNAL_BUS_OK);
	bootstrap_is_unchanged();
	failed_write_status = GO4_INTERNAL_BUS_OK;
	repeated_call_is_inert(false);
}

ZTEST_SUITE(ws9652a_pmu_policy, NULL, NULL, reset, NULL, NULL);
