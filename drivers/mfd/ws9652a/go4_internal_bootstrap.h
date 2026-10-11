/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GO4_INTERNAL_BOOTSTRAP_H
#define GO4_INTERNAL_BOOTSTRAP_H

#include <stdbool.h>
#include <stdint.h>
struct pmu_context;

/* PMU I2C bootstrap with bounded retries. The controller owns route, clocks,
 * gate and peer admission. Writes are not replayed once the value may have
 * reached the PMU. Native STOP_FAILED means data completed but cleanup failed;
 * a transfer error remains distinct if cleanup also fails. */
enum go4_bootstrap_status {
	GO4_BOOTSTRAP_OK = 0,
	GO4_BOOTSTRAP_STOP_FAILED = UINT32_MAX,
	GO4_BOOTSTRAP_TRANSFER_TIMEOUT = 0x410,
	GO4_BOOTSTRAP_ARBITRATION = 0x412,
	GO4_BOOTSTRAP_READ_ADDRESS = 0x413,
	GO4_BOOTSTRAP_WRITE_NACK = 0x414,
	GO4_BOOTSTRAP_CLOCK = 0xB05,
};

uint32_t go4_internal_bootstrap_read(struct pmu_context *ctx, uint8_t index, uint8_t *value,
				     uint32_t *attempts);
uint32_t go4_internal_bootstrap_write(struct pmu_context *ctx, uint8_t index, uint8_t value,
				      uint32_t *attempts);

/* Wait for route changes to settle. Native I2C supplies a bounded clock-aware
 * delay; other controllers use the kernel delay. The final argument is unused. */
bool go4_internal_bootstrap_delay(struct pmu_context *ctx, uint32_t us, uint32_t cycles_per_us);

#endif
