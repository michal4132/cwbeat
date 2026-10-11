/* SPDX-License-Identifier: Apache-2.0 */
/* Bus adapters. PMU registers and policy do not depend on controller MMIO. */
#include "pmu_context.h"
#include "go4_internal_bootstrap.h"
#include <zephyr/drivers/misc/ws9652a_bus.h>
#include <zephyr/drivers/misc/ws9652a_pmu_bridge.h>
#if defined(CONFIG_I2C_WS9652A)
#include <zephyr/drivers/i2c/ws9652a.h>
#endif

static uint32_t bridge_read(const struct pmu_context *ctx, uint8_t index, uint8_t *out)
{
#if defined(CONFIG_WS9652A_PMU_BRIDGE)
	return ws9652a_pmu_bridge_read(ctx->config->bridge, index, out);
#else
	ARG_UNUSED(ctx);
	ARG_UNUSED(index);
	ARG_UNUSED(out);
	return GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND;
#endif
}

static uint32_t bridge_write(const struct pmu_context *ctx, uint8_t index, uint8_t value)
{
#if defined(CONFIG_WS9652A_PMU_BRIDGE)
	return ws9652a_pmu_bridge_write(ctx->config->bridge, index, value);
#else
	ARG_UNUSED(ctx);
	ARG_UNUSED(index);
	ARG_UNUSED(value);
	return GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND;
#endif
}

static uint32_t bridge_reset(const struct pmu_context *ctx)
{
#if defined(CONFIG_WS9652A_PMU_BRIDGE)
	return ws9652a_pmu_bridge_reset(ctx->config->bridge);
#else
	ARG_UNUSED(ctx);
	return GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND;
#endif
}

static uint32_t i2c_reset(const struct pmu_context *ctx)
{
	ARG_UNUSED(ctx);
	/* The controller configures each transfer itself. Check admission before
	 * PMU preparation, including before a possible direct-mode handover. */
#if defined(CONFIG_I2C_WS9652A)
	if (ctx->config->native_i2c) {
		const struct device *bus = ws9652a_i2c_bus(ctx->config->i2c.bus);
		int error = ws9652a_bus_lock(bus, true);
		if (error) {
			return GO4_INTERNAL_BUS_BUSY;
		}
		ws9652a_bus_unlock(bus);
	}
#endif
	return GO4_INTERNAL_BUS_OK;
}

const struct ws9652a_pmu_transport ws9652a_pmu_bridge_transport = {
	.read = bridge_read,
	.write = bridge_write,
	.reset = bridge_reset,
	.direct = true,
};

const struct ws9652a_pmu_transport ws9652a_pmu_i2c_transport = {
	.read = pmu_i2c_read_once,
	.write = pmu_i2c_write_once,
	.reset = i2c_reset,
	.direct = false,
};

uint32_t pmu_transport_control(const struct pmu_context *ctx)
{
#if defined(CONFIG_WS9652A_PMU_BRIDGE)
	if (ctx->config->transport->direct) {
		return ws9652a_pmu_bridge_control(ctx->config->bridge);
	}
#endif
	uint32_t snapshot[8];
	pmu_transport_snapshot(ctx, snapshot);
	return snapshot[3];
}

void pmu_transport_snapshot(const struct pmu_context *ctx, uint32_t out[8])
{
	ARG_UNUSED(ctx);
	for (unsigned i = 0; i < 8; ++i) {
		out[i] = UINT32_MAX;
	}
#if defined(CONFIG_I2C_WS9652A)
	if (ctx->config->native_i2c) {
		(void)ws9652a_i2c_snapshot(ctx->config->i2c.bus, out);
	}
#endif
}
