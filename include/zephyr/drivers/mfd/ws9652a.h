/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZEPHYR_DRIVERS_MFD_WS9652A_H
#define ZEPHYR_DRIVERS_MFD_WS9652A_H
#include <zephyr/device.h>
#include <stdbool.h>
#include <stdint.h>
/* Explicit once-per-reset policy; caller must hold BT and acknowledge DSP hold.
 * Thread context only, after kernel startup. Serialized with local runtime
 * operations; false also covers local lock timeout. Device initialization itself
 * performs no register reads or writes. */
bool ws9652a_pmu_prepare(const struct device *dev);
uint32_t ws9652a_pmu_bus_control(const struct device *dev);

struct ws9652a_pmu_identity {
	uint8_t backend_c7, id_f5, id_f4;
};

/* Experimental, opt-in runtime read (CONFIG_MFD_WS9652A_RUNTIME_READ).
 * Thread context only, after backend setup during prepare(). Serializes the whole
 * C7/F5/F4 sequence against local preparation and other identity callers.
 * This local lock does not exclude BT/DSP. No initialization, retry, policy
 * write or peer hold is performed. Output is unchanged on any error.
 * Returns 0 or negative errno; -EBUSY means local/controller contention,
 * Backend readiness is retained even if the subsequent stock profile fails.
 * -EAGAIN means the backend is unprepared, -ENOTSUP means the option is
 * disabled or the configured transport is not selected.
 */
int ws9652a_pmu_read_identity(const struct device *dev, struct ws9652a_pmu_identity *out);

/* Child-driver operations run under the shared local lock, only after the
 * stock preparation has completed. Callbacks may sleep; the lock is retained.
 * This does not exclude BT/DSP. ISR/pre-kernel/lock refusal invokes no callback.
 * Returns the callback result, -EINVAL for a null callback, -EWOULDBLOCK in an
 * ISR, -EAGAIN before preparation, or -EBUSY on bounded local lock contention.
 */
typedef int (*ws9652a_pmu_operation_t)(const struct device *dev, void *context);
int ws9652a_pmu_transaction(const struct device *dev, ws9652a_pmu_operation_t operation,
			    void *context);

/* Only callable from a transaction callback on the same device. These access
 * the devicetree-selected transport without initialization or retries. Return the native
 * transport status (zero on success); use ws9652a_pmu_bus_error() for errno.
 * Read output is valid only on success. Backend verification also publishes
 * the observed byte on unsupported-route refusal, matching the bus helper.
 */
uint32_t ws9652a_pmu_read_locked(const struct device *dev, uint8_t index, uint8_t *out);
uint32_t ws9652a_pmu_write_locked(const struct device *dev, uint8_t index, uint8_t value);
uint32_t ws9652a_pmu_verify_backend_locked(const struct device *dev, uint8_t *out);
int ws9652a_pmu_bus_error(uint32_t status);
#endif
