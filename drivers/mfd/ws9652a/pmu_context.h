/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WS9652A_PMU_CONTEXT_H
#define WS9652A_PMU_CONTEXT_H
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/mfd/ws9652a.h>
#include <zephyr/drivers/i2c.h>
#include <errno.h>
#include "go4_internal_bus.h"
#include "power_policy.h"
struct ws9652a_pmu_transport {
	uint32_t (*read)(const struct pmu_context *ctx, uint8_t index, uint8_t *out);
	uint32_t (*write)(const struct pmu_context *ctx, uint8_t index, uint8_t value);
	uint32_t (*reset)(const struct pmu_context *ctx);
	bool direct;
};
struct ws9652a_pmu_config {
	const struct ws9652a_pmu_transport *transport;
	const struct device *bridge;
	struct i2c_dt_spec i2c;
	bool native_i2c;
};
struct ws9652a_pmu_data {
	uint32_t bootstrap[GO4_BUS_BOOTSTRAP_WORDS];
	uint32_t diagnostic[POWER_POLICY_SNAPSHOT_WORDS];
	struct k_mutex lock;
	enum power_policy_state preparation_state;
	bool backend_ready;
};
struct pmu_context {
	const struct ws9652a_pmu_config *config;
	struct ws9652a_pmu_data *data;
};

extern const struct ws9652a_pmu_transport ws9652a_pmu_bridge_transport;
extern const struct ws9652a_pmu_transport ws9652a_pmu_i2c_transport;
uint32_t pmu_i2c_read_once(const struct pmu_context *ctx, uint8_t index, uint8_t *out);
uint32_t pmu_i2c_write_once(const struct pmu_context *ctx, uint8_t index, uint8_t value);
uint32_t pmu_transport_control(const struct pmu_context *ctx);
void pmu_transport_snapshot(const struct pmu_context *ctx, uint32_t out[8]);

static inline int pmu_bus_error(uint32_t status)
{
	switch (status) {
	case GO4_INTERNAL_BUS_OK:
		return 0;
	case GO4_INTERNAL_BUS_TIMEOUT:
	case 0x410:
		return -ETIMEDOUT;
	case GO4_INTERNAL_BUS_BUSY:
		return -EBUSY;
	case GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND:
		return -ENOTSUP;
	case 0x412:
		return -EAGAIN;
	case 0x413:
	case 0x414:
		return -ENXIO;
	default:
		return -EIO;
	}
}
#endif
