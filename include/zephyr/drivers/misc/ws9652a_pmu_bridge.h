/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZEPHYR_DRIVERS_MISC_WS9652A_PMU_BRIDGE_H
#define ZEPHYR_DRIVERS_MISC_WS9652A_PMU_BRIDGE_H

#include <zephyr/device.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ws9652a_pmu_bridge_status {
	WS9652A_PMU_BRIDGE_OK = 0,
	WS9652A_PMU_BRIDGE_TIMEOUT = 0xb00,
	WS9652A_PMU_BRIDGE_INVALID_OUTPUT = 0xb01,
	WS9652A_PMU_BRIDGE_BUSY = 0xb02,
	WS9652A_PMU_BRIDGE_STATUS_ERROR = 0xb03,
};

/* Native byte-engine protocol, without PMU register interpretation, backend
 * selection or retries. Each call takes the shared local domain lock, checks
 * for pending controller requests, and restores PRIMASK around the byte phase.
 * Local-only admission preserves experimental access while peers are running;
 * callers requiring peer exclusion must retain a require_held domain lock.
 * Lock/context refusal maps to BUSY. Null read output returns INVALID_OUTPUT
 * without locking or MMIO; every failed read leaves the output unchanged.
 */
uint32_t ws9652a_pmu_bridge_read(const struct device *dev, uint8_t index, uint8_t *out);
uint32_t ws9652a_pmu_bridge_write(const struct device *dev, uint8_t index, uint8_t value);

/* Require a held-peer admission window. Refuse pending requests, then clear
 * prepare/gate/enable in native order and select direction 1. No PMU write.
 */
uint32_t ws9652a_pmu_bridge_reset(const struct device *dev);

/* Raw control observation for diagnostics; no locking or register writes. */
uint32_t ws9652a_pmu_bridge_control(const struct device *dev);
const struct device *ws9652a_pmu_bridge_bus(const struct device *dev);

#ifdef __cplusplus
}
#endif
#endif
