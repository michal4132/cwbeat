/* SPDX-License-Identifier: Apache-2.0 */
#define DT_DRV_COMPAT zgmicro_ws9652a_peer
#include <zephyr/drivers/mfd/ws9652a.h>
#include <zephyr/drivers/misc/ws9652a_bus.h>
#include <zephyr/soc/ws9652a/counter.h>
#include <stdint.h>
#include <errno.h>
#include <zephyr/drivers/peer/ws9652a.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_PINCTRL)
#include <zephyr/drivers/pinctrl.h>
#endif

LOG_MODULE_REGISTER(ws9652a_peer, CONFIG_WS9652A_PEER_LOG_LEVEL);

struct pinctrl_dev_config;

struct peer_config {
	uintptr_t bt, mapping, dsp, controller, policy;
	const struct device *pmu, *register_bus, *watchdog;
	uint32_t flash_offset;
	const struct pinctrl_dev_config *pcfg;
	bool pinctrl_present;
};
struct peer_data {
	uint32_t preflight[IPC_POLICY_SNAPSHOT_WORDS];
	struct k_mutex lifecycle_lock;
};
static bool peer_dsp_hold(const struct peer_config *config);
#define BT_RUN_MASK               UINT32_C(0x0c)
#define DSP_COMMAND_MASK          UINT32_C(0x1f)
#define DSP_HOLD_COMMAND          UINT32_C(0x01)
#define DSP_COMMAND_PENDING       UINT32_C(0x20)
#define DSP_HOLD_ACK              UINT32_C(0x10)
#define DSP_HOLD_POLL_LIMIT       100u
#define PEER_LOCK_TIMEOUT         K_MSEC(50)
#define WORD(address)             (*(volatile uint32_t *)(uintptr_t)(address))

static void peer_log_result(const struct device *dev, const char *action, int error)
{
	/* Emergency helpers never call this. Keep runtime logs outside the local
	 * lifecycle/domain locks and skip interrupt-masked caller contexts. */
	if (!IS_ENABLED(CONFIG_LOG) || k_is_pre_kernel() || k_is_in_isr() ||
	    !arch_cpu_irqs_are_enabled() ||
	    __get_PRIMASK() != 0U) {
		return;
	}
	if (!error) {
		LOG_DBG("%s: %s complete", dev->name, action);
	} else if (error == -EBUSY || error == -EAGAIN || error == -EPERM) {
		LOG_DBG("%d: %s: %s refused", error, dev->name, action);
	} else {
		LOG_ERR("%d: %s: %s failed", error, dev->name, action);
	}
}

static int lifecycle_lock(const struct device *dev, k_timeout_t timeout)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (k_is_pre_kernel()) {
		return -EAGAIN;
	}
	struct peer_data *data = dev->data;
	int error = k_mutex_lock(&data->lifecycle_lock, timeout) ? -EBUSY : 0;
	if (error) {
		peer_log_result(dev, "lifecycle lock", error);
	}
	return error;
}

static void lifecycle_unlock(const struct device *dev)
{
	struct peer_data *data = dev->data;
	k_mutex_unlock(&data->lifecycle_lock);
}

static bool peers_are_held(const struct peer_config *config)
{
	return !(WORD(config->bt) & BT_RUN_MASK) &&
	       (WORD(config->dsp + 8) & (DSP_COMMAND_MASK | DSP_COMMAND_PENDING)) ==
		       DSP_HOLD_COMMAND &&
	       (WORD(config->dsp + 12) & DSP_HOLD_ACK);
}

static bool peers_hold(const struct peer_config *config)
{
	/* TOP 0x4a166..0x4a174: stop BT before the DSP-side hold request.
	 * The official hardware bit names are not available; these operations
	 * reproduce the examined firmware's reset sequence. */
	WORD(config->bt) &= ~BT_RUN_MASK;
	/* The held BT application can no longer feed it. Already disabled is OK. */
	(void)wdt_disable(config->watchdog);
	return peer_dsp_hold(config);
}

static bool peer_dsp_hold(const struct peer_config *config)
{
	WORD((config->dsp + 8)) = (WORD((config->dsp + 8)) & ~DSP_COMMAND_MASK) |
				  DSP_COMMAND_PENDING | DSP_HOLD_COMMAND;
	bool acknowledged = false;
	for (unsigned remaining = DSP_HOLD_POLL_LIMIT; remaining != 0; --remaining) {
		if (WORD((config->dsp + 12)) & DSP_HOLD_ACK) {
			acknowledged = true;
			break;
		}
	}
	WORD((config->dsp + 8)) &= ~DSP_COMMAND_PENDING;
	WORD((config->dsp + 0x20)) = 0;
	WORD((config->dsp + 0x24)) = 0;
	WORD((config->dsp + 0x28)) = 0;
	return acknowledged;
}

bool ws9652a_peer_prepare_power(const struct device *dev)
{
	if (lifecycle_lock(dev, PEER_LOCK_TIMEOUT)) {
		return false;
	}
	const struct peer_config *config = dev->config;
	struct peer_data *data = dev->data;
	data->preflight[IPC_POLICY_STATE] = IPC_POLICY_HOLDING;
	data->preflight[IPC_POLICY_STARTED_CYCLES] = ws9652a_counter_read();
	data->preflight[IPC_POLICY_BT_BEFORE] = WORD(config->bt);
	data->preflight[IPC_POLICY_DSP_CONTROL_BEFORE] = WORD(config->dsp + 8);
	data->preflight[IPC_POLICY_DSP_STATUS_BEFORE] = WORD(config->dsp + 12);
	data->preflight[IPC_POLICY_BUS_BEFORE] = ws9652a_pmu_bus_control(config->pmu);
	uint32_t primask;
	__asm volatile("mrs %0, primask" : "=r"(primask));
	data->preflight[IPC_POLICY_PRIMASK] = primask;
	/* Drain transport users before changing the DSP hold command. Publish a
	 * fresh hardware verification, then drop the domain lock before PMU policy
	 * takes its own lock: lifecycle -> PMU -> register bus is the lock order. */
	int error = ws9652a_bus_lock(config->register_bus, false);
	bool acknowledged = false;
	if (!error) {
		ws9652a_bus_set_peers_held_locked(config->register_bus, false);
		acknowledged = peers_hold(config);
	}
	data->preflight[IPC_POLICY_HOLD_ACK] = acknowledged;
	data->preflight[IPC_POLICY_DSP_CONTROL_AFTER] = WORD(config->dsp + 8);
	data->preflight[IPC_POLICY_DSP_STATUS_AFTER] = WORD(config->dsp + 12);
	data->preflight[IPC_POLICY_BT_AFTER] = WORD(config->bt);
	bool held = acknowledged && !(data->preflight[IPC_POLICY_BT_AFTER] & BT_RUN_MASK) &&
		    (data->preflight[IPC_POLICY_DSP_CONTROL_AFTER] &
		     (DSP_COMMAND_MASK | DSP_COMMAND_PENDING)) == DSP_HOLD_COMMAND &&
		    (data->preflight[IPC_POLICY_DSP_STATUS_AFTER] & DSP_HOLD_ACK);
	if (!error) {
		ws9652a_bus_set_peers_held_locked(config->register_bus, held);
		ws9652a_bus_unlock(config->register_bus);
	}
	bool applied = false;
	if (held) {
		applied = ws9652a_pmu_prepare(config->pmu);
	}
	data->preflight[IPC_POLICY_BUS_AFTER] = ws9652a_pmu_bus_control(config->pmu);
	data->preflight[IPC_POLICY_COMPLETED_CYCLES] = ws9652a_counter_read();
	data->preflight[IPC_POLICY_STATE] = !held     ? IPC_POLICY_HOLD_FAILED
					    : applied ? IPC_POLICY_READY
						      : IPC_POLICY_FAILED;
	lifecycle_unlock(dev);
	peer_log_result(dev, "power preparation",
			error ? error : !held ? (acknowledged ? -EIO : -ETIMEDOUT) :
					       applied ? 0 : -EIO);
	return applied;
}

static int prepare_pins(const struct device *dev)
{
	const struct peer_config *config = dev->config;
	const struct peer_data *data = dev->data;

	/* Peer pads belong to the held-peer startup phase, after PMU policy
	 * and shared-arena initialization, before strap detection and release. */
	if (data->preflight[IPC_POLICY_STATE] != IPC_POLICY_READY ||
	    (WORD(config->bt) & BT_RUN_MASK)) {
		return -EPERM;
	}
	if (!config->pinctrl_present) {
		return 0;
	}
#if defined(CONFIG_PINCTRL)
	return pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
#else
	return -ENOTSUP;
#endif
}

int ws9652a_peer_prepare_pins(const struct device *dev)
{
	int error = lifecycle_lock(dev, PEER_LOCK_TIMEOUT);
	if (error) {
		return error;
	}
	error = prepare_pins(dev);
	lifecycle_unlock(dev);
	peer_log_result(dev, "pin preparation", error);
	return error;
}

uint32_t ws9652a_peer_power_state(const struct device *dev)
{
	const struct peer_data *data = dev->data;
	return data->preflight[IPC_POLICY_STATE];
}
uint32_t ws9652a_peer_bt_control(const struct device *dev)
{
	const struct peer_config *config = dev->config;
	return WORD(config->bt);
}
bool ws9652a_peer_bt_enabled(uint32_t captured)
{
	return (captured & BT_RUN_MASK) != 0;
}
bool ws9652a_peer_mapping_valid(const struct device *dev)
{
	const struct peer_config *config = dev->config;
	return WORD(config->mapping) == config->flash_offset && WORD(config->mapping + 4) == 1;
}
bool ws9652a_peer_hold(const struct device *dev)
{
	if (lifecycle_lock(dev, PEER_LOCK_TIMEOUT)) {
		return false;
	}
	const struct peer_config *config = dev->config;
	bool held = false;
	int error = ws9652a_bus_lock(config->register_bus, false);
	if (!error) {
		ws9652a_bus_set_peers_held_locked(config->register_bus, false);
		bool acknowledged = peers_hold(config);
		held = acknowledged && peers_are_held(config);
		error = held ? 0 : acknowledged ? -EIO : -ETIMEDOUT;
		ws9652a_bus_set_peers_held_locked(config->register_bus, held);
		ws9652a_bus_unlock(config->register_bus);
	}
	lifecycle_unlock(dev);
	peer_log_result(dev, "peer hold", error);
	return held;
}
bool ws9652a_peer_dsp_hold(const struct device *dev)
{
	if (lifecycle_lock(dev, PEER_LOCK_TIMEOUT)) {
		return false;
	}
	const struct peer_config *config = dev->config;
	bool held = false;
	int error = ws9652a_bus_lock(config->register_bus, false);
	if (!error) {
		ws9652a_bus_set_peers_held_locked(config->register_bus, false);
		held = peer_dsp_hold(config);
		error = held ? 0 : -ETIMEDOUT;
		ws9652a_bus_set_peers_held_locked(config->register_bus,
						  held && peers_are_held(config));
		ws9652a_bus_unlock(config->register_bus);
	}
	lifecycle_unlock(dev);
	peer_log_result(dev, "DSP hold", error);
	return held;
}
void ws9652a_peer_bt_release(const struct device *dev, uint32_t captured)
{
	/* The void lifecycle API must not silently lose an authorized release
	 * to ordinary contention. A bus transaction has bounded hardware waits. */
	if (lifecycle_lock(dev, K_FOREVER)) {
		return;
	}
	const struct peer_config *config = dev->config;
	int error;
	do {
		error = ws9652a_bus_lock(config->register_bus, false);
	} while (error == -EBUSY);
	if (!error) {
		ws9652a_bus_set_peers_held_locked(config->register_bus, false);
		WORD(config->bt) = captured | BT_RUN_MASK;
		ws9652a_bus_unlock(config->register_bus);
	}
	lifecycle_unlock(dev);
	peer_log_result(dev, "BT release", error);
}
void ws9652a_peer_bt_stop(const struct device *dev)
{
	const struct peer_config *config = dev->config;
	WORD(config->bt) &= ~BT_RUN_MASK;
}
void ws9652a_peer_controller_stop(const struct device *dev)
{
	const struct peer_config *config = dev->config;
	WORD(config->controller) &= ~UINT32_C(0x0c);
	WORD(config->policy) |= UINT32_C(0x02);
	WORD(config->policy) &= ~UINT32_C(0x0c);
	WORD(config->policy) &= ~UINT32_C(0x0c);
}
/* Immutable address configuration: usable before application BSS/device init. */
static const struct peer_config emergency_config = {
	.bt = DT_REG_ADDR_BY_NAME(DT_NODELABEL(peer_ctrl), bt),
	.mapping = DT_REG_ADDR_BY_NAME(DT_NODELABEL(peer_ctrl), mapping),
	.dsp = DT_REG_ADDR_BY_NAME(DT_NODELABEL(peer_ctrl), dsp),
	.watchdog = DEVICE_DT_GET(DT_PHANDLE(DT_NODELABEL(peer_ctrl), watchdog)),
	.controller = DT_REG_ADDR_BY_NAME(DT_NODELABEL(peer_ctrl), controller),
	.policy = DT_REG_ADDR_BY_NAME(DT_NODELABEL(peer_ctrl), policy),
};
bool ws9652a_peer_hold_emergency(void)
{
	return peers_hold(&emergency_config);
}
bool ws9652a_peer_dsp_hold_emergency(void)
{
	return peer_dsp_hold(&emergency_config);
}
void ws9652a_peer_snapshot_emergency(struct ws9652a_peer_snapshot *out)
{
	out->bt = WORD(emergency_config.bt);
	out->dsp_control = WORD(emergency_config.dsp + 8);
	out->dsp_status = WORD(emergency_config.dsp + 12);
}
static int init(const struct device *dev)
{
	const struct peer_config *config = dev->config;
	struct peer_data *data = dev->data;
	if (!device_is_ready(config->watchdog)) {
		LOG_ERR("%d: %s: watchdog %s not ready", -ENODEV, dev->name,
			config->watchdog->name);
		return -ENODEV;
	}
	if (!device_is_ready(config->pmu)) {
		LOG_ERR("%d: %s: PMU %s not ready", -ENODEV, dev->name, config->pmu->name);
		return -ENODEV;
	}
	if (!device_is_ready(config->register_bus)) {
		LOG_ERR("%d: %s: register bus %s not ready", -ENODEV, dev->name,
			config->register_bus->name);
		return -ENODEV;
	}
	k_mutex_init(&data->lifecycle_lock);
	LOG_DBG("%s: initialized", dev->name);
	return 0;
}
#if defined(CONFIG_PINCTRL)
#define PEER_PINCTRL_DEFINE(inst)                                                                  \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, pinctrl_0), (PINCTRL_DT_INST_DEFINE(inst);), ())
#define PEER_PINCTRL_CONFIG(inst)                                                                  \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, pinctrl_0), (PINCTRL_DT_INST_DEV_CONFIG_GET(inst)),   \
		    (NULL))
#else
#define PEER_PINCTRL_DEFINE(inst)
#define PEER_PINCTRL_CONFIG(inst) NULL
#endif
#define DEFINE(inst)                                                                               \
	PEER_PINCTRL_DEFINE(inst)                                                                  \
	static const struct peer_config config_##inst = {                                          \
		.bt = DT_INST_REG_ADDR_BY_NAME(inst, bt),                                          \
		.mapping = DT_INST_REG_ADDR_BY_NAME(inst, mapping),                                \
		.dsp = DT_INST_REG_ADDR_BY_NAME(inst, dsp),                                        \
		.watchdog = DEVICE_DT_GET(DT_INST_PHANDLE(inst, watchdog)),                        \
		.controller = DT_INST_REG_ADDR_BY_NAME(inst, controller),                          \
		.policy = DT_INST_REG_ADDR_BY_NAME(inst, policy),                                  \
		.pmu = DEVICE_DT_GET(DT_INST_PHANDLE(inst, pmu)),                                  \
		.register_bus = DEVICE_DT_GET(DT_INST_PHANDLE(inst, register_bus)),                \
		.flash_offset = DT_INST_PROP(inst, stock_flash_offset),                            \
		.pcfg = PEER_PINCTRL_CONFIG(inst),                                                 \
		.pinctrl_present = DT_INST_NODE_HAS_PROP(inst, pinctrl_0)};                        \
	static struct peer_data data_##inst;                                                       \
	DEVICE_DT_INST_DEFINE(inst, init, NULL, &data_##inst, &config_##inst, PRE_KERNEL_1, 41,    \
			      NULL);
DT_INST_FOREACH_STATUS_OKAY(DEFINE)
