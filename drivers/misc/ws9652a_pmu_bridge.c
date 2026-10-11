/* SPDX-License-Identifier: Apache-2.0 */
/* Dedicated byte engine: stock TOP 0x44ff8/0x45020 and 0x44f04/0x44f90.
 * No PMU registers, C7 mode selection or alternate-controller operations here.
 */
#define DT_DRV_COMPAT zgmicro_ws9652a_pmu_bridge

#include <zephyr/drivers/misc/ws9652a_bus.h>
#include <zephyr/drivers/misc/ws9652a_pmu_bridge.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <cmsis_core.h>

LOG_MODULE_REGISTER(ws9652a_pmu_bridge, CONFIG_WS9652A_PMU_BRIDGE_LOG_LEVEL);

struct ws9652a_pmu_bridge_config {
	uintptr_t base;
	const struct device *bus;
};

enum {
	REG_CONTROL = 0,
	REG_WRITE_DATA = 4,
	REG_READ_DATA = 8,
	REG_STATUS = 0x0c,
	REG_DIRECTION = 0x10,
	REG_INDEX = 0x14,
	CONTROL_ENABLE = BIT(0),
	CONTROL_READ = BIT(1),
	CONTROL_WRITE = BIT(2),
	CONTROL_PREPARE = BIT(5),
	CONTROL_GATE = BIT(6),
	TRANSFER_MASK = CONTROL_READ | CONTROL_WRITE,
	STATUS_COMPLETE = BIT(0),
	STATUS_ERROR = BIT(1),
	DIRECTION_INTERNAL = 1,
	POLL_LIMIT = 3000,
	STOCK_PRELUDE_ITERATIONS = 18,
	SETUP_DELAY_NOPS = 192,
};

static void control_set(const struct ws9652a_pmu_bridge_config *config, uint32_t bits)
{
	sys_write32(sys_read32(config->base + REG_CONTROL) | bits, config->base + REG_CONTROL);
}

static void control_clear(const struct ws9652a_pmu_bridge_config *config, uint32_t bits)
{
	sys_write32(sys_read32(config->base + REG_CONTROL) & ~bits, config->base + REG_CONTROL);
}

static void setup(const struct ws9652a_pmu_bridge_config *config)
{
	control_set(config, CONTROL_PREPARE);
	sys_write32(DIRECTION_INTERNAL, config->base + REG_DIRECTION);
	control_set(config, CONTROL_GATE);
	control_set(config, CONTROL_ENABLE);
	/* Preserve repeated writes from the native preparation helper. */
	control_set(config, CONTROL_GATE);
	ws9652a_bus_gate_set_locked(config->bus, 1);
	control_set(config, CONTROL_PREPARE);
	for (unsigned remaining = STOCK_PRELUDE_ITERATIONS; remaining; --remaining) {
		__asm volatile("nop");
	}
	/* Finite delay of at least 1us at the highest known 192MHz core rate.
	 * Slower clocks extend it. No timer/DWT configuration or extra MMIO.
	 */
	for (unsigned remaining = SETUP_DELAY_NOPS; remaining; --remaining) {
		__asm volatile("nop");
	}
}

static uint32_t read_byte(const struct ws9652a_pmu_bridge_config *config, uint8_t index,
			  uint8_t *out)
{
	/* Native PRIMASK, rather than BASEPRI, covers only the byte phase. */
	uint32_t key = __get_PRIMASK();

	__disable_irq();
	sys_write32(index, config->base + REG_INDEX);
	control_set(config, CONTROL_READ);
	uint32_t result = WS9652A_PMU_BRIDGE_OK;
	unsigned remaining = POLL_LIMIT;

	while (!(sys_read32(config->base + REG_STATUS) & STATUS_COMPLETE)) {
		if (!--remaining) {
			break;
		}
	}
	if (remaining) {
		*out = sys_read32(config->base + REG_READ_DATA);
	} else {
		result = WS9652A_PMU_BRIDGE_TIMEOUT;
	}
	remaining = POLL_LIMIT;
	while (sys_read32(config->base + REG_STATUS) & STATUS_COMPLETE) {
		if (!--remaining) {
			break;
		}
	}
	if (!remaining) {
		result = WS9652A_PMU_BRIDGE_TIMEOUT;
	} else if (sys_read32(config->base + REG_STATUS) & STATUS_ERROR) {
		/* Retain native precedence: the final error observation can replace
		 * a timeout from the first completion wait.
		 */
		result = WS9652A_PMU_BRIDGE_STATUS_ERROR;
	}
	control_clear(config, CONTROL_READ);
	__set_PRIMASK(key);
	return result;
}

static uint32_t write_byte(const struct ws9652a_pmu_bridge_config *config, uint8_t index,
			   uint8_t value)
{
	uint32_t key = __get_PRIMASK();

	__disable_irq();
	sys_write32(value, config->base + REG_WRITE_DATA);
	sys_write32(index, config->base + REG_INDEX);
	control_set(config, CONTROL_WRITE);
	unsigned remaining = POLL_LIMIT;

	while (!(sys_read32(config->base + REG_STATUS) & STATUS_COMPLETE)) {
		if (!--remaining) {
			break;
		}
	}
	if (remaining) {
		remaining = POLL_LIMIT;
		while (sys_read32(config->base + REG_STATUS) & STATUS_COMPLETE) {
			if (!--remaining) {
				break;
			}
		}
	}
	control_clear(config, CONTROL_WRITE);
	/* The native writer samples the final error after restoring PRIMASK. */
	__set_PRIMASK(key);
	if (!remaining) {
		return WS9652A_PMU_BRIDGE_TIMEOUT;
	}
	return sys_read32(config->base + REG_STATUS) & STATUS_ERROR
		       ? WS9652A_PMU_BRIDGE_STATUS_ERROR
		       : WS9652A_PMU_BRIDGE_OK;
}

static void cleanup(const struct ws9652a_pmu_bridge_config *config)
{
	ws9652a_bus_gate_set_locked(config->bus, 0);
	control_clear(config, CONTROL_GATE);
	control_clear(config, CONTROL_PREPARE);
}

uint32_t ws9652a_pmu_bridge_read(const struct device *dev, uint8_t index, uint8_t *out)
{
	if (!out) {
		LOG_DBG("0x%x: %s: read 0x%02x: null output", WS9652A_PMU_BRIDGE_INVALID_OUTPUT,
			dev->name, index);
		return WS9652A_PMU_BRIDGE_INVALID_OUTPUT;
	}
	const struct ws9652a_pmu_bridge_config *config = dev->config;
	int error = ws9652a_bus_lock(config->bus, false);

	if (error) {
		LOG_DBG("%d: %s: read 0x%02x: domain lock failed", error, dev->name, index);
		return WS9652A_PMU_BRIDGE_BUSY;
	}
	uint32_t status = WS9652A_PMU_BRIDGE_BUSY;

	if (!(sys_read32(config->base + REG_CONTROL) & TRANSFER_MASK)) {
		uint8_t value = 0;

		setup(config);
		status = read_byte(config, index, &value);
		cleanup(config);
		if (status == WS9652A_PMU_BRIDGE_OK) {
			*out = value;
		}
	}
	ws9652a_bus_unlock(config->bus);
	if (status != WS9652A_PMU_BRIDGE_OK) {
		LOG_DBG("0x%x: %s: read 0x%02x failed", status, dev->name, index);
	}
	return status;
}

uint32_t ws9652a_pmu_bridge_write(const struct device *dev, uint8_t index, uint8_t value)
{
	const struct ws9652a_pmu_bridge_config *config = dev->config;
	int error = ws9652a_bus_lock(config->bus, false);

	if (error) {
		LOG_DBG("%d: %s: write 0x%02x: domain lock failed", error, dev->name, index);
		return WS9652A_PMU_BRIDGE_BUSY;
	}
	uint32_t status = WS9652A_PMU_BRIDGE_BUSY;

	if (!(sys_read32(config->base + REG_CONTROL) & TRANSFER_MASK)) {
		setup(config);
		status = write_byte(config, index, value);
		cleanup(config);
	}
	ws9652a_bus_unlock(config->bus);
	if (status != WS9652A_PMU_BRIDGE_OK) {
		LOG_DBG("0x%x: %s: write 0x%02x failed", status, dev->name, index);
	}
	return status;
}

uint32_t ws9652a_pmu_bridge_reset(const struct device *dev)
{
	const struct ws9652a_pmu_bridge_config *config = dev->config;
	int error = ws9652a_bus_lock(config->bus, true);

	if (error) {
		LOG_DBG("%d: %s: reset: domain lock failed", error, dev->name);
		return WS9652A_PMU_BRIDGE_BUSY;
	}
	uint32_t status = WS9652A_PMU_BRIDGE_BUSY;

	if (!(sys_read32(config->base + REG_CONTROL) & TRANSFER_MASK)) {
		control_clear(config, CONTROL_PREPARE);
		control_clear(config, CONTROL_GATE);
		control_clear(config, CONTROL_ENABLE);
		sys_write32(DIRECTION_INTERNAL, config->base + REG_DIRECTION);
		status = WS9652A_PMU_BRIDGE_OK;
	}
	ws9652a_bus_unlock(config->bus);
	if (status != WS9652A_PMU_BRIDGE_OK) {
		LOG_DBG("0x%x: %s: reset failed", status, dev->name);
	}
	return status;
}

uint32_t ws9652a_pmu_bridge_control(const struct device *dev)
{
	const struct ws9652a_pmu_bridge_config *config = dev->config;

	return sys_read32(config->base + REG_CONTROL);
}

const struct device *ws9652a_pmu_bridge_bus(const struct device *dev)
{
	const struct ws9652a_pmu_bridge_config *config = dev->config;

	return config->bus;
}

static int ws9652a_pmu_bridge_init(const struct device *dev)
{
	const struct device *bus = ws9652a_pmu_bridge_bus(dev);

	if (!device_is_ready(bus)) {
		LOG_ERR("%d: %s: init: register bus %s not ready", -ENODEV, dev->name, bus->name);
		return -ENODEV;
	}
	return 0;
}

#define WS9652A_PMU_BRIDGE_DEFINE(inst)                                                            \
	static const struct ws9652a_pmu_bridge_config bridge_config_##inst = {                     \
		.base = DT_INST_REG_ADDR(inst),                                                    \
		.bus = DEVICE_DT_GET(DT_INST_PHANDLE(inst, register_bus)),                         \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, ws9652a_pmu_bridge_init, NULL, NULL, &bridge_config_##inst,    \
			      PRE_KERNEL_1, 35, NULL);

DT_INST_FOREACH_STATUS_OKAY(WS9652A_PMU_BRIDGE_DEFINE)
