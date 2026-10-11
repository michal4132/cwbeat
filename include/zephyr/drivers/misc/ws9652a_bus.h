/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZEPHYR_DRIVERS_MISC_WS9652A_BUS_H
#define ZEPHYR_DRIVERS_MISC_WS9652A_BUS_H

#include <zephyr/device.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Serialize the direct and I2C engines sharing a gate. Thread context only.
 * The mutex is recursive; each successful lock requires one unlock. Requiring
 * held peers also checks the admission flag on recursive entry. Admission starts
 * closed and is published only by the peer lifecycle owner after verified hold.
 * Returns -EWOULDBLOCK in ISR, -EAGAIN before kernel startup, or -EBUSY for a
 * closed held-peer window or bounded local lock contention. No MMIO is performed.
 * A local-only lock (require_held=false) does not exclude BT or DSP.
 */
int ws9652a_bus_lock(const struct device *dev, bool require_held);
void ws9652a_bus_unlock(const struct device *dev);

/* Caller holds the domain lock. The peer lifecycle owner must close admission
 * before releasing either peer, retaining this lock through the hardware change.
 */
void ws9652a_bus_set_peers_held_locked(const struct device *dev, bool held);

/* Raw bit-band gate access, only while holding the domain lock. Locking alone
 * does not change the gate; each transport retains its verified write ordering.
 */
uint32_t ws9652a_bus_gate_get_locked(const struct device *dev);
void ws9652a_bus_gate_set_locked(const struct device *dev, uint32_t value);

#ifdef __cplusplus
}
#endif
#endif
