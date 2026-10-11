/* SPDX-License-Identifier: Apache-2.0 */
#define DT_DRV_COMPAT zgmicro_ws9652a_register_bus

#include <zephyr/drivers/misc/ws9652a_bus.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <errno.h>

LOG_MODULE_REGISTER(ws9652a_register_bus, CONFIG_WS9652A_REGISTER_BUS_LOG_LEVEL);

#define WS9652A_BUS_LOCK_TIMEOUT K_MSEC(50)

struct ws9652a_bus_config {
	uintptr_t gate;
};

struct ws9652a_bus_data {
	struct k_mutex lock;
	bool peers_held;
};

int ws9652a_bus_lock(const struct device *dev, bool require_held)
{
	if (k_is_in_isr()) {
		LOG_DBG("%d: %s: lock refused in ISR", -EWOULDBLOCK, dev->name);
		return -EWOULDBLOCK;
	}
	if (k_is_pre_kernel()) {
		LOG_DBG("%d: %s: lock refused before kernel startup", -EAGAIN, dev->name);
		return -EAGAIN;
	}
	struct ws9652a_bus_data *data = dev->data;

	if (k_mutex_lock(&data->lock, WS9652A_BUS_LOCK_TIMEOUT)) {
		LOG_DBG("%d: %s: lock timed out", -EBUSY, dev->name);
		return -EBUSY;
	}
	if (require_held && !data->peers_held) {
		k_mutex_unlock(&data->lock);
		LOG_DBG("%d: %s: lock refused: peers-held admission closed", -EBUSY, dev->name);
		return -EBUSY;
	}
	return 0;
}

void ws9652a_bus_unlock(const struct device *dev)
{
	struct ws9652a_bus_data *data = dev->data;

	k_mutex_unlock(&data->lock);
}

void ws9652a_bus_set_peers_held_locked(const struct device *dev, bool held)
{
	struct ws9652a_bus_data *data = dev->data;
	bool changed = data->peers_held != held;

	data->peers_held = held;
	if (changed) {
		LOG_DBG("%s: peers-held admission %s", dev->name, held ? "opened" : "closed");
	}
}

uint32_t ws9652a_bus_gate_get_locked(const struct device *dev)
{
	const struct ws9652a_bus_config *config = dev->config;

	return sys_read32(config->gate);
}

void ws9652a_bus_gate_set_locked(const struct device *dev, uint32_t value)
{
	const struct ws9652a_bus_config *config = dev->config;

	sys_write32(value, config->gate);
}

static int ws9652a_bus_init(const struct device *dev)
{
	struct ws9652a_bus_data *data = dev->data;

	k_mutex_init(&data->lock);
	data->peers_held = false;
	return 0;
}

#define WS9652A_BUS_DEFINE(inst)                                                                   \
	static const struct ws9652a_bus_config bus_config_##inst = {                               \
		.gate = DT_INST_REG_ADDR(inst),                                                    \
	};                                                                                         \
	static struct ws9652a_bus_data bus_data_##inst;                                            \
	DEVICE_DT_INST_DEFINE(inst, ws9652a_bus_init, NULL, &bus_data_##inst, &bus_config_##inst,  \
			      PRE_KERNEL_1, 30, NULL);

DT_INST_FOREACH_STATUS_OKAY(WS9652A_BUS_DEFINE)
