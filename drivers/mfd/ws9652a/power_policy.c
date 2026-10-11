/* SPDX-License-Identifier: Apache-2.0 */
/* Original TOP 0x49BFC, canonical scatter policy and ATS 0x200 flag zero.
 * The native oracle compares every byte-register read/write, including the
 * repeated 0x4407C revision classifiers; no stock private RAM is reused.
 */
#include "power_policy.h"
#include "pmu_context.h"
#include "go4_internal_bus.h"
#include "go4_internal_bootstrap.h"

enum {
	REG_POLICY_00 = 0x00,
	REG_POLICY_01 = 0x01,
	REG_POLICY_02 = 0x02,
	REG_POLICY_04 = 0x04,
	REG_POLICY_05 = 0x05,
	REG_POLICY_07 = 0x07,
	REG_POLICY_09 = 0x09,
	REG_POLICY_0C = 0x0C,
	REG_POLICY_0D = 0x0D,
	REG_POLICY_12 = 0x12,
	REG_POLICY_1B = 0x1B,
	REG_STATUS_C0 = 0xC0,
	REG_ACK_C1 = 0xC1,
	REG_STATUS_C2 = 0xC2,
	REG_BACKEND_C7 = 0xC7,
	REG_ID_F4 = 0xF4,
	REG_ID_F5 = 0xF5,
	PROFILE_POLICY_0C = 0x01,
	PROFILE_POLICY_0D = 0x01,
	PROFILE_POLICY_02 = 0x51,
	PROFILE_POLICY_12 = 0x01,
	PROFILE_POLICY_00 = 0x0C,
	PROFILE_POLICY_07 = 0x02,
	PROFILE_POLICY_01 = 0x1A,
	PROFILE_POLICY_04 = 0x5A,
	PROFILE_CHECK_04 = 0x56,
	PROFILE_POLICY_05 = 0x01,
	PROFILE_POLICY_1B = 0x01,
	POLICY_09_PRESERVED_MASK = 0x03,
	POLICY_02_ENABLE_MASK = 0x08,
	POLICY_02_ZERO_STATE_MASK = 0x04,
	STATUS_C2_CLEAR_MASK = 0x08,
	STATUS_C0_STATE_SHIFT = 2,
	STATUS_C0_STATE_MASK = 3,
	STATUS_C0_CHECK_MASK = 0x10,
	ID_F5_NEEDS_F4 = 0x01,
	ID_F4_CLASS_14 = 0xBB,
	ID_F5_CLASS_14_FIRST = 0x02,
	ID_F5_CLASS_14_LAST = 0x04,
	ID_F4_CLASS_16 = 0xBA,
	ID_F5_CLASS_12_FIRST = 0x11,
	ID_F5_CLASS_12_LAST = 0x12,
	ID_F5_CLASS_11_FIRST = 0x0C,
	ID_F5_CLASS_11_LAST = 0x0D,
	ID_F5_CLASS_15_FIRST = 0xA0,
	ID_F5_CLASS_15_LAST = 0xCF,
	ID_CLASS_11 = 0x11,
	ID_CLASS_12 = 0x12,
	ID_CLASS_14 = 0x14,
	ID_CLASS_15 = 0x15,
	ID_CLASS_16 = 0x16,
	ID_CLASS_UNKNOWN = 0x1D,
	ID_BYTE_UNOBSERVED = 0xFF,
	/* Number of original 0x4407C calls before each class-0x14 operation. */
	CHECKS_NONE = 0,
	CHECKS_SINGLE = 1,
	CHECKS_INITIAL_0C = 7,
	CHECKS_INITIAL_0D = 7,
	CHECKS_POLICY_00 = 5,
	CHECKS_STATUS_C0 = 6,
	CHECKS_FINAL = 3,
};

struct fixed_operation {
	uint8_t index, value, revision_checks;
};
static const struct fixed_operation fixed_policy[] = {
	{REG_POLICY_0C, PROFILE_POLICY_0C, CHECKS_INITIAL_0C},
	{REG_POLICY_0D, PROFILE_POLICY_0D, CHECKS_INITIAL_0D},
	{REG_POLICY_02, PROFILE_POLICY_02, CHECKS_NONE},
	{REG_POLICY_12, PROFILE_POLICY_12, CHECKS_SINGLE},
	{REG_POLICY_00, PROFILE_POLICY_00, CHECKS_POLICY_00},
	{REG_POLICY_07, PROFILE_POLICY_07, CHECKS_SINGLE},
	{REG_POLICY_01, PROFILE_POLICY_01, CHECKS_SINGLE},
};
#define diagnostic (ctx->data->diagnostic)

static void set_state(struct pmu_context *ctx, enum power_policy_state state)
{
	ctx->data->preparation_state = state;
	diagnostic[POWER_POLICY_STATE] = state;
}

static bool read_register(struct pmu_context *ctx, enum power_policy_step step, uint8_t index,
			  uint8_t *out)
{
	diagnostic[POWER_POLICY_LAST_STEP] = step;
	diagnostic[POWER_POLICY_REGISTER_INDEX] = index;
	diagnostic[POWER_POLICY_REGISTER_VALUE] = POWER_POLICY_UNOBSERVED;
	++diagnostic[POWER_POLICY_OPERATION_COUNTS];
	uint32_t status = go4_internal_bus_read(ctx, index, out);
	diagnostic[POWER_POLICY_BUS_STATUS] = status;
	if (status != GO4_INTERNAL_BUS_OK) {
		set_state(ctx, POWER_POLICY_BUS_ERROR);
		return false;
	}
	diagnostic[POWER_POLICY_REGISTER_VALUE] = *out;
	return true;
}

static bool write_register(struct pmu_context *ctx, enum power_policy_step step, uint8_t index,
			   uint8_t value)
{
	diagnostic[POWER_POLICY_LAST_STEP] = step;
	diagnostic[POWER_POLICY_REGISTER_INDEX] = index;
	diagnostic[POWER_POLICY_REGISTER_VALUE] = value;
	diagnostic[POWER_POLICY_OPERATION_COUNTS] += UINT32_C(1) << POWER_POLICY_WRITE_COUNT_SHIFT;
	uint32_t status = go4_internal_bus_write(ctx, index, value);
	diagnostic[POWER_POLICY_BUS_STATUS] = status;
	if (status != GO4_INTERNAL_BUS_OK) {
		set_state(ctx, POWER_POLICY_BUS_ERROR);
		return false;
	}
	return true;
}

static bool revision_checks(struct pmu_context *ctx, unsigned count)
{
	while (count--) {
		uint8_t f5, f4 = ID_BYTE_UNOBSERVED;
		if (!read_register(ctx, POWER_POLICY_STEP_IDENTITY_F5, REG_ID_F5, &f5)) {
			return false;
		}
		if (f5 == ID_F5_NEEDS_F4 &&
		    !read_register(ctx, POWER_POLICY_STEP_IDENTITY_F4, REG_ID_F4, &f4)) {
			return false;
		}
		bool supported = (f5 == ID_F5_NEEDS_F4 && f4 == ID_F4_CLASS_14) ||
				 (f5 >= ID_F5_CLASS_14_FIRST && f5 <= ID_F5_CLASS_14_LAST);
		uint8_t classified = supported ? ID_CLASS_14 : ID_CLASS_UNKNOWN;
		if (f5 == ID_F5_NEEDS_F4 && f4 == ID_F4_CLASS_16) {
			classified = ID_CLASS_16;
		} else if (f5 >= ID_F5_CLASS_12_FIRST && f5 <= ID_F5_CLASS_12_LAST) {
			classified = ID_CLASS_12;
		} else if (f5 >= ID_F5_CLASS_11_FIRST && f5 <= ID_F5_CLASS_11_LAST) {
			classified = ID_CLASS_11;
		} else if (f5 >= ID_F5_CLASS_15_FIRST && f5 <= ID_F5_CLASS_15_LAST) {
			classified = ID_CLASS_15;
		}
		diagnostic[POWER_POLICY_IDENTITY] =
			f5 | (uint32_t)f4 << POWER_POLICY_IDENTITY_F4_SHIFT |
			(uint32_t)classified << POWER_POLICY_IDENTITY_CLASS_SHIFT;
		if (!supported) {
			set_state(ctx, POWER_POLICY_UNSUPPORTED_REVISION);
			return false;
		}
	}
	return true;
}

static bool read_status(struct pmu_context *ctx, enum power_policy_step step,
			enum power_policy_word field)
{
	uint8_t value;
	if (!revision_checks(ctx, CHECKS_STATUS_C0) ||
	    !read_register(ctx, step, REG_STATUS_C0, &value)) {
		return false;
	}
	diagnostic[field] = value;
	return true;
}

static bool temporary_status_check(struct pmu_context *ctx)
{
	if (!revision_checks(ctx, CHECKS_SINGLE)) {
		return false;
	}
	/* A transfer reporting failure may still have applied its data. Restore
	 * the normal value even when the initial temporary write returns error. */
	if (write_register(ctx, POWER_POLICY_STEP_CHECK_04, REG_POLICY_04, PROFILE_CHECK_04) &&
	    read_status(ctx, POWER_POLICY_STEP_CHECK_C0_FIRST, POWER_POLICY_CHECK_C0_FIRST) &&
	    read_status(ctx, POWER_POLICY_STEP_CHECK_C0_SECOND, POWER_POLICY_CHECK_C0_SECOND) &&
	    revision_checks(ctx, CHECKS_SINGLE) &&
	    write_register(ctx, POWER_POLICY_STEP_RESTORE_04, REG_POLICY_04, PROFILE_POLICY_04)) {
		diagnostic[POWER_POLICY_CLEANUP_STATUS] = GO4_INTERNAL_BUS_OK;
		return true;
	}
	/* Keep the first failed operation intact; this separate field reports
	 * the one cleanup attempt and does not turn a failed policy into success. */
	diagnostic[POWER_POLICY_OPERATION_COUNTS] += UINT32_C(1) << POWER_POLICY_WRITE_COUNT_SHIFT;
	diagnostic[POWER_POLICY_CLEANUP_STATUS] =
		go4_internal_bus_write(ctx, REG_POLICY_04, PROFILE_POLICY_04);
	return false;
}

bool power_policy_start(struct pmu_context *ctx)
{
	if (ctx->data->preparation_state != POWER_POLICY_IDLE) {
		return ctx->data->preparation_state == POWER_POLICY_DONE;
	}
	for (unsigned field = POWER_POLICY_BACKEND_C7; field < POWER_POLICY_OPERATION_COUNTS;
	     ++field) {
		diagnostic[field] = POWER_POLICY_UNOBSERVED;
	}
	for (unsigned field = POWER_POLICY_INIT_STATUS; field < POWER_POLICY_SNAPSHOT_WORDS;
	     ++field) {
		diagnostic[field] = POWER_POLICY_UNOBSERVED;
	}
	set_state(ctx, POWER_POLICY_RUNNING);
	diagnostic[POWER_POLICY_LAST_STEP] = POWER_POLICY_STEP_BUS_INIT;
	diagnostic[POWER_POLICY_REGISTER_INDEX] = REG_BACKEND_C7;
	diagnostic[POWER_POLICY_REGISTER_VALUE] = POWER_POLICY_UNOBSERVED;
	uint8_t value;
	uint32_t status = go4_internal_bus_init(ctx, &value);
	diagnostic[POWER_POLICY_INIT_STATUS] = status;
	diagnostic[POWER_POLICY_BUS_STATUS] = status;
	if (status == GO4_INTERNAL_BUS_OK || status == GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND) {
		diagnostic[POWER_POLICY_BACKEND_C7] = value;
		diagnostic[POWER_POLICY_REGISTER_VALUE] = value;
	}
	/* Alternate access can time out before any backend write while direct
	 * access becomes available. The policy owns this one read-only recovery
	 * decision; retain the strict initializer's failure and hardware snapshot.
	 */
	const uint32_t *bootstrap = ctx->data->bootstrap;
	if (status != GO4_INTERNAL_BUS_OK &&
	    bootstrap[GO4_BUS_BOOTSTRAP_STATE] == GO4_BUS_BOOTSTRAP_FAILED &&
	    bootstrap[GO4_BUS_BOOTSTRAP_READ_STATUS] == GO4_BOOTSTRAP_TRANSFER_TIMEOUT &&
	    bootstrap[GO4_BUS_BOOTSTRAP_READ_ATTEMPTS] > 0 &&
	    bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS] == 0) {
		diagnostic[POWER_POLICY_LAST_STEP] = POWER_POLICY_STEP_BACKEND_VERIFY;
		diagnostic[POWER_POLICY_REGISTER_VALUE] = POWER_POLICY_UNOBSERVED;
		++diagnostic[POWER_POLICY_OPERATION_COUNTS];
		status = go4_internal_bus_verify_backend(ctx, &value);
		diagnostic[POWER_POLICY_RECOVERY_STATUS] = status;
		diagnostic[POWER_POLICY_BUS_STATUS] = status;
		if (status == GO4_INTERNAL_BUS_OK || status == GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND) {
			diagnostic[POWER_POLICY_RECOVERY_C7] = value;
			diagnostic[POWER_POLICY_BACKEND_C7] = value;
			diagnostic[POWER_POLICY_REGISTER_VALUE] = value;
		}
	}
	if (status != GO4_INTERNAL_BUS_OK) {
		set_state(ctx, POWER_POLICY_BUS_ERROR);
		return false;
	}
	/* Identity reads need the backend, independently of the stock profile. */
	ctx->data->backend_ready = true;
	for (unsigned i = 0; i < sizeof(fixed_policy) / sizeof(fixed_policy[0]); ++i) {
		const struct fixed_operation *operation = &fixed_policy[i];
		if (!revision_checks(ctx, operation->revision_checks) ||
		    !write_register(ctx, POWER_POLICY_STEP_FIXED, operation->index,
				    operation->value)) {
			return false;
		}
	}
	if (!revision_checks(ctx, CHECKS_SINGLE) ||
	    !read_register(ctx, POWER_POLICY_STEP_READ_09, REG_POLICY_09, &value) ||
	    !write_register(ctx, POWER_POLICY_STEP_WRITE_09, REG_POLICY_09,
			    value & POLICY_09_PRESERVED_MASK) ||
	    !revision_checks(ctx, CHECKS_SINGLE) ||
	    !write_register(ctx, POWER_POLICY_STEP_DEFAULT_04, REG_POLICY_04, PROFILE_POLICY_04) ||
	    !read_status(ctx, POWER_POLICY_STEP_INITIAL_C0, POWER_POLICY_INITIAL_C0)) {
		return false;
	}
	if ((diagnostic[POWER_POLICY_INITIAL_C0] & STATUS_C0_CHECK_MASK) &&
	    !temporary_status_check(ctx)) {
		return false;
	}
	if (!read_register(ctx, POWER_POLICY_STEP_READ_02, REG_POLICY_02, &value) ||
	    !write_register(ctx, POWER_POLICY_STEP_WRITE_02, REG_POLICY_02,
			    value | POLICY_02_ENABLE_MASK)) {
		return false;
	}
	/* 0x49DA0 classifies the FIRST C0 read. Its later two check reads do not
	 * change the state controlling this branch. Only zero versus nonzero is
	 * consumed here; stock maps 0/1/2/3 to 0/3/2/1 in its private RAM. */
	if (((diagnostic[POWER_POLICY_INITIAL_C0] >> STATUS_C0_STATE_SHIFT) &
	     STATUS_C0_STATE_MASK) == 0) {
		if (!revision_checks(ctx, CHECKS_SINGLE) ||
		    !read_register(ctx, POWER_POLICY_STEP_READ_02_ZERO, REG_POLICY_02, &value) ||
		    !revision_checks(ctx, CHECKS_SINGLE) ||
		    !write_register(ctx, POWER_POLICY_STEP_WRITE_02_ZERO, REG_POLICY_02,
				    value | POLICY_02_ZERO_STATE_MASK)) {
			return false;
		}
	}
	if (!read_register(ctx, POWER_POLICY_STEP_ACK_C0, REG_STATUS_C0, &value)) {
		return false;
	}
	diagnostic[POWER_POLICY_ACK_C0] = value;
	if (!write_register(ctx, POWER_POLICY_STEP_ACK_C1, REG_ACK_C1, value) ||
	    !read_register(ctx, POWER_POLICY_STEP_READ_C2, REG_STATUS_C2, &value)) {
		return false;
	}
	diagnostic[POWER_POLICY_C2_BEFORE] = value;
	if (!write_register(ctx, POWER_POLICY_STEP_WRITE_C2, REG_STATUS_C2,
			    value & ~STATUS_C2_CLEAR_MASK) ||
	    !revision_checks(ctx, CHECKS_FINAL) ||
	    !write_register(ctx, POWER_POLICY_STEP_FINAL_05, REG_POLICY_05, PROFILE_POLICY_05) ||
	    !revision_checks(ctx, CHECKS_FINAL) ||
	    !write_register(ctx, POWER_POLICY_STEP_FINAL_1B, REG_POLICY_1B, PROFILE_POLICY_1B) ||
	    !revision_checks(ctx, CHECKS_FINAL) ||
	    !read_register(ctx, POWER_POLICY_STEP_READ_1B, REG_POLICY_1B, &value)) {
		return false;
	}
	set_state(ctx, POWER_POLICY_DONE);
	return true;
}
