/* SPDX-License-Identifier: Apache-2.0 */
/* PMU bootstrap policy over an I2C device. START/STOP, route selection,
 * clocks and gate manipulation belong to the I2C controller driver.
 */
#include "pmu_context.h"
#include "go4_internal_bootstrap.h"
#include <zephyr/drivers/i2c/ws9652a.h>

enum { MAX_ATTEMPTS = 5, RETRY_DELAY_US = 150 };

static uint32_t transfer_once(const struct pmu_context *ctx, uint8_t index,
			     uint8_t *value, bool read, bool *write_delivered)
{
	uint8_t bytes[] = {index, read ? 0 : *value};
	uint8_t received = 0;
	struct i2c_msg messages[2] = {
		{.buf = bytes, .len = read ? 1 : 2, .flags = read ? 0 : I2C_MSG_STOP},
		{.buf = &received, .len = 1, .flags = I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP},
	};
	struct ws9652a_i2c_result result = {0};
	int error;
#if defined(CONFIG_I2C_WS9652A)
	if (ctx->config->native_i2c) {
		error = ws9652a_i2c_transfer_detailed(ctx->config->i2c.bus, messages,
			read ? 2 : 1, ctx->config->i2c.addr, &result);
	} else
#endif
	{
		error = i2c_transfer_dt(&ctx->config->i2c, messages, read ? 2 : 1);
		result.transfer_error = error;
		result.data_completed = error ? 0 : 2;
		/* Generic I2C cannot report whether a failed write reached the
		 * device. Do not authorize a replay of a mode-changing write. */
		if (!read) {
			*write_delivered = true;
		}
	}
	if (!read && result.transfer_error == 0 && result.data_completed == 2) {
		*write_delivered = true;
	}
	if (!read && result.phase == WS9652A_I2C_PHASE_DATA && result.data_completed >= 1) {
		/* The register index was acknowledged and the value was issued.
		 * A lost ACK cannot establish whether the PMU applied that value. */
		*write_delivered = true;
	}
	if (!error) {
		if (read) {
			*value = received;
		}
		return GO4_BOOTSTRAP_OK;
	}
	if (result.transfer_error == 0 && result.stop_error != 0) {
		return GO4_BOOTSTRAP_STOP_FAILED;
	}
	switch (error) {
	case -ETIMEDOUT:
		return GO4_BOOTSTRAP_TRANSFER_TIMEOUT;
	case -EAGAIN:
		return GO4_BOOTSTRAP_ARBITRATION;
	case -ENXIO:
		return read && result.message_index == 1
			? GO4_BOOTSTRAP_READ_ADDRESS : GO4_BOOTSTRAP_WRITE_NACK;
	case -EBUSY:
		return GO4_INTERNAL_BUS_BUSY;
	default:
		return GO4_BOOTSTRAP_CLOCK;
	}
}

uint32_t pmu_i2c_read_once(const struct pmu_context *ctx, uint8_t index, uint8_t *out)
{
	if (out == NULL) {
		return GO4_INTERNAL_BUS_INVALID_OUTPUT;
	}
	bool delivered = false;
	return transfer_once(ctx, index, out, true, &delivered);
}

uint32_t pmu_i2c_write_once(const struct pmu_context *ctx, uint8_t index, uint8_t value)
{
	bool delivered = false;
	return transfer_once(ctx, index, &value, false, &delivered);
}

bool go4_internal_bootstrap_delay(struct pmu_context *ctx, uint32_t us, uint32_t unused)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(unused);
#if defined(CONFIG_I2C_WS9652A)
	if (ctx->config->native_i2c) {
		return ws9652a_i2c_delay(ctx->config->i2c.bus, us) == 0;
	}
#endif
	k_busy_wait(us);
	return true;
}

static uint32_t operation(struct pmu_context *ctx, uint8_t index, uint8_t *value,
			  bool read, uint32_t *attempts)
{
	*attempts = 0;
	uint32_t status = GO4_BOOTSTRAP_TRANSFER_TIMEOUT;
	for (unsigned attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt) {
		bool delivered = false;
		*attempts = attempt;
		status = transfer_once(ctx, index, value, read, &delivered);
		if (status == GO4_BOOTSTRAP_OK || delivered ||
		    status == GO4_BOOTSTRAP_CLOCK || status == GO4_INTERNAL_BUS_BUSY) {
			break;
		}
		if (!go4_internal_bootstrap_delay(ctx, attempt * RETRY_DELAY_US, 0)) {
			return GO4_BOOTSTRAP_CLOCK;
		}
	}
	return status;
}

uint32_t go4_internal_bootstrap_read(struct pmu_context *ctx, uint8_t index, uint8_t *value,
				     uint32_t *attempts)
{
	return operation(ctx, index, value, true, attempts);
}

uint32_t go4_internal_bootstrap_write(struct pmu_context *ctx, uint8_t index, uint8_t value,
				      uint32_t *attempts)
{
	return operation(ctx, index, &value, false, attempts);
}
