/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZEPHYR_DRIVERS_PEER_WS9652A_H
#define ZEPHYR_DRIVERS_PEER_WS9652A_H
#include <zephyr/device.h>
#include <stdbool.h>
#include <stdint.h>
enum ipc_policy_preflight_state {
	IPC_POLICY_IDLE,
	IPC_POLICY_HOLDING,
	IPC_POLICY_READY,
	IPC_POLICY_HOLD_FAILED,
	IPC_POLICY_FAILED,
};
enum ipc_policy_preflight_word {
	IPC_POLICY_STATE,
	IPC_POLICY_STARTED_CYCLES,
	IPC_POLICY_COMPLETED_CYCLES,
	IPC_POLICY_BT_BEFORE,
	IPC_POLICY_DSP_CONTROL_BEFORE,
	IPC_POLICY_DSP_STATUS_BEFORE,
	IPC_POLICY_DSP_CONTROL_AFTER,
	IPC_POLICY_DSP_STATUS_AFTER,
	IPC_POLICY_HOLD_ACK,
	IPC_POLICY_BT_AFTER,
	IPC_POLICY_BUS_BEFORE,
	IPC_POLICY_BUS_AFTER,
	IPC_POLICY_PRIMASK,
	IPC_POLICY_RESERVED,
	IPC_POLICY_SNAPSHOT_WORDS,
};

/* Thread-context lifecycle calls, after kernel startup. Prepare holds peers
 * before explicit PMU policy. Prepare, pin setup, hold, DSP hold and release
 * share the lifecycle lock. Hold transitions and release also serialize through
 * the register-bus domain. Verified holds open its admission;
 * release closes admission before changing BT run bits and waits for current
 * transfers to finish. Preserve captured control across preflight; publish
 * arena/straps and DMB before release. Bool operations return false on bounded
 * lock contention. Emergency helpers never open register-bus admission. */
uint32_t ws9652a_peer_bt_control(const struct device *dev);
bool ws9652a_peer_bt_enabled(uint32_t captured);
bool ws9652a_peer_mapping_valid(const struct device *dev);
bool ws9652a_peer_prepare_power(const struct device *dev);
/* Apply the peer's default pinctrl state after PMU/arena preparation, while BT
 * remains held. Returns -EPERM outside that phase, -ENOTSUP if a state is
 * declared with pinctrl disabled, or the pinctrl error. No state is a no-op. */
int ws9652a_peer_prepare_pins(const struct device *dev);
uint32_t ws9652a_peer_power_state(const struct device *dev);
bool ws9652a_peer_hold(const struct device *dev);
void ws9652a_peer_bt_release(const struct device *dev, uint32_t captured);
bool ws9652a_peer_dsp_hold(const struct device *dev);
/* Fault/reset context: bounded direct hardware hold, no device data or scheduler. */
bool ws9652a_peer_hold_emergency(void);
bool ws9652a_peer_dsp_hold_emergency(void);
/* Low-level shutdown helpers also bypass lifecycle locks. */
void ws9652a_peer_bt_stop(const struct device *dev);
void ws9652a_peer_controller_stop(const struct device *dev);
struct ws9652a_peer_snapshot {
	uint32_t bt, dsp_control, dsp_status;
};
void ws9652a_peer_snapshot_emergency(struct ws9652a_peer_snapshot *out);
#endif
