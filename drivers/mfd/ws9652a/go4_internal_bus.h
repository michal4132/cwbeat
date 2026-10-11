/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GO4_INTERNAL_BUS_H
#define GO4_INTERNAL_BUS_H

#include <stdint.h>
struct pmu_context;

enum go4_internal_bus_status {
	GO4_INTERNAL_BUS_OK = 0,
	GO4_INTERNAL_BUS_TIMEOUT = 0xB00,
	GO4_INTERNAL_BUS_INVALID_OUTPUT = 0xB01,
	/* Replacement-only preflight refusal; stock wrappers have no busy guard. */
	GO4_INTERNAL_BUS_BUSY = 0xB02,
	GO4_INTERNAL_BUS_STATUS_ERROR = 0xB03,
	/* Neither direct access nor the alternate bootstrap selected this backend. */
	GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND = 0xB04,
};

enum go4_internal_bootstrap_word {
	GO4_BUS_BOOTSTRAP_STATE,
	GO4_BUS_BOOTSTRAP_DIRECT_STATUS,
	GO4_BUS_BOOTSTRAP_DIRECT_C7,
	GO4_BUS_BOOTSTRAP_READ_STATUS,
	GO4_BUS_BOOTSTRAP_WRITE_STATUS,
	GO4_BUS_BOOTSTRAP_CONFIRM_STATUS,
	GO4_BUS_BOOTSTRAP_READ_ATTEMPTS,
	GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS,
	GO4_BUS_BOOTSTRAP_BEFORE_C7,
	GO4_BUS_BOOTSTRAP_AFTER_C7,
	GO4_BUS_BOOTSTRAP_CONFIRM_INITIAL_STATUS,
	GO4_BUS_BOOTSTRAP_CONFIRM_INITIAL_C7,
	GO4_BUS_BOOTSTRAP_CONFIRM_ATTEMPTS,
	GO4_BUS_BOOTSTRAP_CONFIRM_DELAY_STATUS,
	GO4_BUS_BOOTSTRAP_BEFORE_CLOCK_SOURCE,
	GO4_BUS_BOOTSTRAP_BEFORE_CLOCK_SELECT,
	GO4_BUS_BOOTSTRAP_BEFORE_CLOCK_DIVIDER,
	GO4_BUS_BOOTSTRAP_BEFORE_ALTERNATE_CONTROL,
	GO4_BUS_BOOTSTRAP_BEFORE_ALTERNATE_COMMAND,
	GO4_BUS_BOOTSTRAP_BEFORE_ALTERNATE_STATUS,
	GO4_BUS_BOOTSTRAP_BEFORE_ALTERNATE_DIRECTION,
	GO4_BUS_BOOTSTRAP_BEFORE_GATE,
	GO4_BUS_BOOTSTRAP_AFTER_CLOCK_SOURCE,
	GO4_BUS_BOOTSTRAP_AFTER_CLOCK_SELECT,
	GO4_BUS_BOOTSTRAP_AFTER_CLOCK_DIVIDER,
	GO4_BUS_BOOTSTRAP_AFTER_ALTERNATE_CONTROL,
	GO4_BUS_BOOTSTRAP_AFTER_ALTERNATE_COMMAND,
	GO4_BUS_BOOTSTRAP_AFTER_ALTERNATE_STATUS,
	GO4_BUS_BOOTSTRAP_AFTER_ALTERNATE_DIRECTION,
	GO4_BUS_BOOTSTRAP_AFTER_GATE,
	GO4_BUS_BOOTSTRAP_WORDS,
};
enum go4_internal_bootstrap_state {
	GO4_BUS_BOOTSTRAP_IDLE,
	GO4_BUS_BOOTSTRAP_FAST,
	GO4_BUS_BOOTSTRAP_RUNNING,
	GO4_BUS_BOOTSTRAP_DONE,
	GO4_BUS_BOOTSTRAP_FAILED,
};

/* One PMU byte-register operation through the selected transport. The caller
 * holds the PMU mutex. Transport providers own controller sequencing and peer
 * admission. Reads publish output only on success. No initialization or C7
 * selection is performed here. */
uint32_t go4_internal_bus_read(struct pmu_context *ctx, uint8_t index, uint8_t *out);

uint32_t go4_internal_bus_write(struct pmu_context *ctx, uint8_t index, uint8_t value);

/* Read C7 once through the selected transport. Direct mode requires bit 2 set
 * and bit 3 clear; I2C requires bit 2 clear. This never changes modes or retries.
 * Output is valid for OK and UNSUPPORTED_BACKEND only. */
uint32_t go4_internal_bus_verify_backend(struct pmu_context *ctx, uint8_t *backend_c7);

/* Prepare the selected PMU mode while native peer admission is held.
 * Direct mode uses stock C7 selection (old | 4) & ~8 through bootstrap I2C
 * when necessary, then confirms through the bridge. An acknowledged write
 * with STOP failure allows five further bounded read-only confirmations.
 * I2C mode optionally clears C7 bit 2 through the bridge when inheriting
 * direct mode, then verifies through I2C. No possibly delivered C7 write is
 * replayed. Diagnostics retain bootstrap status and provider snapshots.
 * Null output performs no transport operation. */
uint32_t go4_internal_bus_init(struct pmu_context *ctx, uint8_t *backend_c7);

#endif
