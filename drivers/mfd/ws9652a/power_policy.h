/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GO4_POWER_POLICY_H
#define GO4_POWER_POLICY_H

#include <stdbool.h>
#include <stdint.h>
struct pmu_context;

enum power_policy_state {
	POWER_POLICY_IDLE,
	POWER_POLICY_RUNNING,
	POWER_POLICY_DONE,
	POWER_POLICY_BUS_ERROR,
	POWER_POLICY_UNSUPPORTED_REVISION,
};

enum power_policy_step {
	POWER_POLICY_STEP_NONE,
	POWER_POLICY_STEP_BUS_INIT,
	POWER_POLICY_STEP_IDENTITY_F5,
	POWER_POLICY_STEP_IDENTITY_F4,
	POWER_POLICY_STEP_FIXED,
	POWER_POLICY_STEP_READ_09,
	POWER_POLICY_STEP_WRITE_09,
	POWER_POLICY_STEP_DEFAULT_04,
	POWER_POLICY_STEP_INITIAL_C0,
	POWER_POLICY_STEP_CHECK_04,
	POWER_POLICY_STEP_CHECK_C0_FIRST,
	POWER_POLICY_STEP_CHECK_C0_SECOND,
	POWER_POLICY_STEP_RESTORE_04,
	POWER_POLICY_STEP_READ_02,
	POWER_POLICY_STEP_WRITE_02,
	POWER_POLICY_STEP_READ_02_ZERO,
	POWER_POLICY_STEP_WRITE_02_ZERO,
	POWER_POLICY_STEP_ACK_C0,
	POWER_POLICY_STEP_ACK_C1,
	POWER_POLICY_STEP_READ_C2,
	POWER_POLICY_STEP_WRITE_C2,
	POWER_POLICY_STEP_FINAL_05,
	POWER_POLICY_STEP_FINAL_1B,
	POWER_POLICY_STEP_READ_1B,
	POWER_POLICY_STEP_BACKEND_VERIFY,
};

enum power_policy_word {
	POWER_POLICY_STATE,
	POWER_POLICY_LAST_STEP,
	POWER_POLICY_REGISTER_INDEX,
	POWER_POLICY_REGISTER_VALUE,
	POWER_POLICY_BUS_STATUS,
	POWER_POLICY_BACKEND_C7,
	POWER_POLICY_IDENTITY,
	POWER_POLICY_INITIAL_C0,
	POWER_POLICY_CHECK_C0_FIRST,
	POWER_POLICY_CHECK_C0_SECOND,
	POWER_POLICY_ACK_C0,
	POWER_POLICY_C2_BEFORE,
	POWER_POLICY_CLEANUP_STATUS,
	POWER_POLICY_OPERATION_COUNTS,
	/* Appended: preserve the original fourteen diagnostic word offsets. */
	POWER_POLICY_INIT_STATUS,
	POWER_POLICY_RECOVERY_STATUS,
	POWER_POLICY_RECOVERY_C7,
	POWER_POLICY_SNAPSHOT_WORDS,
};

#define POWER_POLICY_UNOBSERVED           UINT32_MAX
#define POWER_POLICY_IDENTITY_F4_SHIFT    8u
#define POWER_POLICY_IDENTITY_CLASS_SHIFT 16u
#define POWER_POLICY_WRITE_COUNT_SHIFT    16u

/* Execute once per reset, before releasing either peer. The caller must
 * establish exclusive internal-bus ownership by holding BT and DSP.
 *
 * This is the stock 0x49BFC policy for the canonical configuration and
 * class-0x14 identity, retaining the original repeated revision reads.
 * It omits stock private RAM, OS synchronization and IRQ7 enable; it is not a complete
 * charging driver. The canonical ATS 0x200 optional flag is zero.
 * All transfers are bounded by go4_internal_bus. A failed operation prevents
 * BT release and is retained. If temporary 04=56 was attempted, a failed
 * check makes one best-effort 04=5A cleanup; other writes are not rolled back.
 * A failed initializer permits one direct C7 verification only after an
 * alternate read timed out, with at least one read attempt and no write
 * attempt. Verified direct access resumes the ordinary identity checks before
 * any policy write. The initial result, verification result and valid C7 value
 * are retained separately; an unattempted verification stays UNOBSERVED.
 * Bootstrap diagnostics remain unchanged, including their failed state.
 * Repeated calls do not issue transactions and return true only after DONE.
 */
bool power_policy_start(struct pmu_context *ctx);

#endif
