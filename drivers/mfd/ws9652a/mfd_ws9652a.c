/* SPDX-License-Identifier: Apache-2.0 */
#define DT_DRV_COMPAT zgmicro_ws9652a_pmu
#include <zephyr/drivers/mfd/ws9652a.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include "pmu_context.h"

LOG_MODULE_REGISTER(ws9652a_pmu, CONFIG_MFD_WS9652A_LOG_LEVEL);

#define PMU_INIT_PRIORITY  40
#define PMU_LOCK_TIMEOUT   K_MSEC(50)
#define PMU_ID_F5          0xf5
#define PMU_ID_F4          0xf4

static void log_preparation(const struct device *dev, bool ready)
{
	const struct ws9652a_pmu_config *config = dev->config;
	const struct ws9652a_pmu_data *data = dev->data;
	const uint32_t *diagnostic = data->diagnostic;
	const uint32_t *bootstrap = data->bootstrap;
	const char *transport = config->transport->direct ? "bridge" : "i2c";

	if (ready) {
		LOG_INF("%s: %s: prepared: C7=0x%02x", dev->name, transport,
			diagnostic[POWER_POLICY_BACKEND_C7]);
	} else {
		int error = data->preparation_state == POWER_POLICY_UNSUPPORTED_REVISION
			? -ENOTSUP : pmu_bus_error(diagnostic[POWER_POLICY_BUS_STATUS]);

		LOG_ERR("%d: %s: %s: prepare failed: state=%u step=%u reg=0x%02x status=0x%x",
			error, dev->name, transport, data->preparation_state,
			diagnostic[POWER_POLICY_LAST_STEP], diagnostic[POWER_POLICY_REGISTER_INDEX],
			diagnostic[POWER_POLICY_BUS_STATUS]);
	}
	LOG_DBG("%s: backend: state=%u direct=0x%x read=0x%x/%u write=0x%x/%u confirm=0x%x/%u",
		dev->name, bootstrap[GO4_BUS_BOOTSTRAP_STATE],
		bootstrap[GO4_BUS_BOOTSTRAP_DIRECT_STATUS], bootstrap[GO4_BUS_BOOTSTRAP_READ_STATUS],
		bootstrap[GO4_BUS_BOOTSTRAP_READ_ATTEMPTS], bootstrap[GO4_BUS_BOOTSTRAP_WRITE_STATUS],
		bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS],
		bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_STATUS],
		bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_ATTEMPTS]);
	LOG_DBG("%s: policy: identity=0x%x init=0x%x recovery=0x%x cleanup=0x%x", dev->name,
		diagnostic[POWER_POLICY_IDENTITY], diagnostic[POWER_POLICY_INIT_STATUS],
		diagnostic[POWER_POLICY_RECOVERY_STATUS], diagnostic[POWER_POLICY_CLEANUP_STATUS]);
}

bool ws9652a_pmu_prepare(const struct device *dev)
{
	struct pmu_context ctx = {dev->config, dev->data};

	if (k_is_in_isr() || k_is_pre_kernel()) {
		return false;
	}
	if (k_mutex_lock(&ctx.data->lock, PMU_LOCK_TIMEOUT)) {
		LOG_DBG("%d: %s: prepare lock failed", -EBUSY, dev->name);
		return false;
	}
	bool first = ctx.data->preparation_state == POWER_POLICY_IDLE;
	bool ready = power_policy_start(&ctx);
	k_mutex_unlock(&ctx.data->lock);
	/* Preparation diagnostics are immutable after the once-per-reset attempt.
	 * Report only that attempt, after all transport and PMU locks are released. */
	if (first) {
		log_preparation(dev, ready);
	}
	return ready;
}

int ws9652a_pmu_read_identity(const struct device *dev, struct ws9652a_pmu_identity *out)
{
	if (!out) {
		return -EINVAL;
	}
	if (!IS_ENABLED(CONFIG_MFD_WS9652A_RUNTIME_READ)) {
		return -ENOTSUP;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (k_is_pre_kernel()) {
		return -EAGAIN;
	}
	struct pmu_context ctx = {dev->config, dev->data};
	if (k_mutex_lock(&ctx.data->lock, PMU_LOCK_TIMEOUT)) {
		LOG_DBG("%d: %s: identity lock failed", -EBUSY, dev->name);
		return -EBUSY;
	}
	int err = -EAGAIN;
	if (ctx.data->backend_ready) {
		struct ws9652a_pmu_identity sample;
		uint32_t status = go4_internal_bus_verify_backend(&ctx, &sample.backend_c7);
		if (!status) {
			status = go4_internal_bus_read(&ctx, PMU_ID_F5, &sample.id_f5);
		}
		if (!status) {
			status = go4_internal_bus_read(&ctx, PMU_ID_F4, &sample.id_f4);
		}
		err = pmu_bus_error(status);
		if (!err) {
			*out = sample;
		}
	}
	k_mutex_unlock(&ctx.data->lock);
	if (err) {
		LOG_DBG("%d: %s: identity read failed", err, dev->name);
	}
	return err;
}

int ws9652a_pmu_transaction(const struct device *dev, ws9652a_pmu_operation_t operation,
			    void *context)
{
	if (!operation) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	if (k_is_pre_kernel()) {
		return -EAGAIN;
	}
	struct ws9652a_pmu_data *data = dev->data;
	if (k_mutex_lock(&data->lock, PMU_LOCK_TIMEOUT)) {
		LOG_DBG("%d: %s: transaction lock failed", -EBUSY, dev->name);
		return -EBUSY;
	}
	bool ready = data->preparation_state == POWER_POLICY_DONE;
	int error = ready ? operation(dev, context) : -EAGAIN;
	k_mutex_unlock(&data->lock);
	return error;
}

uint32_t ws9652a_pmu_read_locked(const struct device *dev, uint8_t index, uint8_t *out)
{
	struct pmu_context ctx = {dev->config, dev->data};
	return go4_internal_bus_read(&ctx, index, out);
}

uint32_t ws9652a_pmu_write_locked(const struct device *dev, uint8_t index, uint8_t value)
{
	struct pmu_context ctx = {dev->config, dev->data};
	return go4_internal_bus_write(&ctx, index, value);
}

uint32_t ws9652a_pmu_verify_backend_locked(const struct device *dev, uint8_t *out)
{
	struct pmu_context ctx = {dev->config, dev->data};
	return go4_internal_bus_verify_backend(&ctx, out);
}

int ws9652a_pmu_bus_error(uint32_t status)
{
	return pmu_bus_error(status);
}

uint32_t ws9652a_pmu_bus_control(const struct device *dev)
{
	struct pmu_context ctx = {dev->config, dev->data};
	return pmu_transport_control(&ctx);
}

static int init(const struct device *dev)
{
	const struct ws9652a_pmu_config *config = dev->config;
	struct ws9652a_pmu_data *data = dev->data;

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("%d: %s: I2C bus not ready", -ENODEV, dev->name);
		return -ENODEV;
	}
	if (config->bridge != NULL && !device_is_ready(config->bridge)) {
		LOG_ERR("%d: %s: bridge not ready", -ENODEV, dev->name);
		return -ENODEV;
	}
	k_mutex_init(&data->lock);
	LOG_DBG("%s: %s transport ready", dev->name,
		config->transport->direct ? "bridge" : "i2c");
	return 0;
}

#define PMU_I2C_NODE(inst)                                                                        \
	COND_CODE_1(DT_INST_ON_BUS(inst, i2c), (DT_BUS(DT_DRV_INST(inst))),                     \
		    (DT_INST_PHANDLE(inst, bootstrap_i2c_bus)))
#define PMU_BRIDGE(inst)                                                                          \
	COND_CODE_1(DT_INST_ON_BUS(inst, i2c),                                                  \
		    (DEVICE_DT_GET_OR_NULL(DT_INST_PHANDLE(inst, direct_bridge))),             \
		    (DEVICE_DT_GET(DT_PARENT(DT_DRV_INST(inst)))))
#define PMU_TRANSPORT(inst)                                                                       \
	COND_CODE_1(DT_INST_ON_BUS(inst, i2c), (&ws9652a_pmu_i2c_transport),                     \
		    (&ws9652a_pmu_bridge_transport))
#define PMU_ADDRESS(inst)                                                                         \
	COND_CODE_1(DT_INST_ON_BUS(inst, i2c), (DT_INST_REG_ADDR(inst)), (0x34))
#define PMU_NATIVE_I2C(inst)                                                                      \
	DT_NODE_HAS_COMPAT(PMU_I2C_NODE(inst), zgmicro_ws9652a_i2c_channel)
#define PMU_INIT_LEVEL(inst)                                                                      \
	COND_CODE_1(PMU_NATIVE_I2C(inst), (PRE_KERNEL_1), (POST_KERNEL))
#define PMU_PRIORITY(inst)                                                                        \
	COND_CODE_1(PMU_NATIVE_I2C(inst), (PMU_INIT_PRIORITY), (CONFIG_MFD_INIT_PRIORITY))

#define DEFINE(inst)                                                                              \
	BUILD_ASSERT(PMU_ADDRESS(inst) == 0x34, "WS9652A PMU has fixed I2C address 0x34");        \
	static const struct ws9652a_pmu_config config_##inst = {                                 \
		.transport = PMU_TRANSPORT(inst),                                               \
		.bridge = PMU_BRIDGE(inst),                                                     \
		.i2c = {.bus = DEVICE_DT_GET(PMU_I2C_NODE(inst)), .addr = PMU_ADDRESS(inst)},     \
		.native_i2c = PMU_NATIVE_I2C(inst),                                             \
	};                                                                                     \
	static struct ws9652a_pmu_data data_##inst;                                              \
	DEVICE_DT_INST_DEFINE(inst, init, NULL, &data_##inst, &config_##inst,                    \
			      PMU_INIT_LEVEL(inst), PMU_PRIORITY(inst), NULL);
DT_INST_FOREACH_STATUS_OKAY(DEFINE)
