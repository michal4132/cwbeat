/* SPDX-License-Identifier: Apache-2.0 */
/* PMU register access and C7 mode selection. Controller protocols live in
 * the I2C and direct-bridge drivers; both paths reach the same PMU registers.
 */
#include "pmu_context.h"
#include "go4_internal_bootstrap.h"

enum {
	BACKEND_REGISTER = 0xc7,
	BACKEND_INTERNAL_ENABLED = 1u << 2,
	/* Physical meaning of bit 3 is unknown; retain the stock mode-3 value. */
	BACKEND_ALTERNATE_SELECTED = 1u << 3,
	BACKEND_VERIFY_RETRIES = 5,
	STOCK_RETRY_BACKOFF_US = 150,
	STOCK_TRANSFER_POLL_US = 10,
};
#define bootstrap (ctx->data->bootstrap)

static bool direct_selected(uint8_t value)
{
	return (value & (BACKEND_INTERNAL_ENABLED | BACKEND_ALTERNATE_SELECTED)) ==
		BACKEND_INTERNAL_ENABLED;
}

static void capture_hardware(struct pmu_context *ctx, enum go4_internal_bootstrap_word first)
{
	pmu_transport_snapshot(ctx, &bootstrap[first]);
}

uint32_t go4_internal_bus_read(struct pmu_context *ctx, uint8_t index, uint8_t *out)
{
	return ctx->config->transport->read(ctx, index, out);
}

uint32_t go4_internal_bus_write(struct pmu_context *ctx, uint8_t index, uint8_t value)
{
	return ctx->config->transport->write(ctx, index, value);
}

uint32_t go4_internal_bus_verify_backend(struct pmu_context *ctx, uint8_t *backend_c7)
{
	uint32_t status = go4_internal_bus_read(ctx, BACKEND_REGISTER, backend_c7);
	if (status != GO4_INTERNAL_BUS_OK) {
		return status;
	}
	bool selected = ctx->config->transport->direct
		? direct_selected(*backend_c7)
		: !(*backend_c7 & BACKEND_INTERNAL_ENABLED);
	return selected ? GO4_INTERNAL_BUS_OK : GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND;
}

static uint32_t prepare_i2c(struct pmu_context *ctx, uint8_t *backend_c7)
{
	uint8_t value = 0;
	uint32_t status = go4_internal_bootstrap_read(ctx, BACKEND_REGISTER, &value,
		&bootstrap[GO4_BUS_BOOTSTRAP_READ_ATTEMPTS]);
	bootstrap[GO4_BUS_BOOTSTRAP_READ_STATUS] = status;
	if (status == GO4_BOOTSTRAP_OK) {
		bootstrap[GO4_BUS_BOOTSTRAP_BEFORE_C7] = value;
		*backend_c7 = value;
		if (!(value & BACKEND_INTERNAL_ENABLED)) {
			return GO4_INTERNAL_BUS_OK;
		}
	}
	/* Optional bridge permits handover from an inherited direct mode. This
	 * reverses stock mode-3 selection without duplicating the PMU device. */
	if (ctx->config->bridge == NULL) {
		return status == GO4_BOOTSTRAP_OK ? GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND : status;
	}
	status = ws9652a_pmu_bridge_transport.read(ctx, BACKEND_REGISTER, &value);
	bootstrap[GO4_BUS_BOOTSTRAP_DIRECT_STATUS] = status;
	if (status != GO4_INTERNAL_BUS_OK) {
		return status;
	}
	bootstrap[GO4_BUS_BOOTSTRAP_DIRECT_C7] = value;
	bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS] = 1;
	status = ws9652a_pmu_bridge_transport.write(ctx, BACKEND_REGISTER,
		value & ~BACKEND_INTERNAL_ENABLED);
	bootstrap[GO4_BUS_BOOTSTRAP_WRITE_STATUS] = status;
	/* A mode-changing write can affect its own completion. Never replay it;
	 * only successful readback through the selected transport authorizes use. */
	uint32_t confirmed = go4_internal_bus_verify_backend(ctx, backend_c7);
	bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_ATTEMPTS] = 1;
	bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_STATUS] = confirmed;
	bootstrap[GO4_BUS_BOOTSTRAP_AFTER_C7] = confirmed == GO4_INTERNAL_BUS_OK
		? *backend_c7 : UINT32_MAX;
	return confirmed == GO4_INTERNAL_BUS_OK ? confirmed
		: status == GO4_INTERNAL_BUS_OK ? confirmed : status;
}

static bool confirm_backend(struct pmu_context *ctx, uint32_t write_status, uint8_t *backend_c7)
{
	for (unsigned retry = 0; retry <= BACKEND_VERIFY_RETRIES; ++retry) {
		if (retry) {
			/* Stock 0x45234 repeats even an acknowledged C7 write after a
			 * failed STOP. Five subsequent NACKs each incur a 10us transfer
			 * poll and 150*n us backoff. Preserve that bounded observation
			 * window (2300us total) with reads only; never replay C7. This
			 * tests settling time without assuming stock's extra controller
			 * setups are required. Delays run outside the byte IRQ lock. */
			bool waited = go4_internal_bootstrap_delay(
				ctx, retry * STOCK_RETRY_BACKOFF_US + STOCK_TRANSFER_POLL_US, 0);
			bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_DELAY_STATUS] =
				waited ? GO4_BOOTSTRAP_OK : GO4_BOOTSTRAP_CLOCK;
			if (!waited) {
				break;
			}
		}
		uint8_t value = 0;
		uint32_t status = go4_internal_bus_read(ctx, BACKEND_REGISTER, &value);
		uint32_t sampled = status == GO4_INTERNAL_BUS_OK ? value : UINT32_MAX;
		++bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_ATTEMPTS];
		bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_STATUS] = status;
		bootstrap[GO4_BUS_BOOTSTRAP_AFTER_C7] = sampled;
		if (!retry) {
			bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_INITIAL_STATUS] = status;
			bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_INITIAL_C7] = sampled;
		}
		if (status == GO4_INTERNAL_BUS_OK && direct_selected(value)) {
			*backend_c7 = value;
			return true;
		}
		if (write_status != GO4_BOOTSTRAP_STOP_FAILED ||
		    (status != GO4_INTERNAL_BUS_OK && status != GO4_INTERNAL_BUS_STATUS_ERROR)) {
			break;
		}
	}
	return false;
}

uint32_t go4_internal_bus_init(struct pmu_context *ctx, uint8_t *backend_c7)
{
	if (!backend_c7) {
		return GO4_INTERNAL_BUS_INVALID_OUTPUT;
	}
	for (unsigned i = 0; i < GO4_BUS_BOOTSTRAP_WORDS; ++i) {
		bootstrap[i] = UINT32_MAX;
	}
	bootstrap[GO4_BUS_BOOTSTRAP_READ_ATTEMPTS] = 0;
	bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS] = 0;
	bootstrap[GO4_BUS_BOOTSTRAP_CONFIRM_ATTEMPTS] = 0;
	bootstrap[GO4_BUS_BOOTSTRAP_STATE] = GO4_BUS_BOOTSTRAP_RUNNING;
	capture_hardware(ctx, GO4_BUS_BOOTSTRAP_BEFORE_CLOCK_SOURCE);
	uint32_t result = ctx->config->transport->reset(ctx);
	if (result != GO4_INTERNAL_BUS_OK) {
		goto failed;
	}
	*backend_c7 = 0;
	if (!ctx->config->transport->direct) {
		result = prepare_i2c(ctx, backend_c7);
		bootstrap[GO4_BUS_BOOTSTRAP_STATE] = result == GO4_INTERNAL_BUS_OK
			? GO4_BUS_BOOTSTRAP_DONE : GO4_BUS_BOOTSTRAP_FAILED;
		goto finished;
	}
	result = go4_internal_bus_read(ctx, BACKEND_REGISTER, backend_c7);
	bootstrap[GO4_BUS_BOOTSTRAP_DIRECT_STATUS] = result;
	if (result == GO4_INTERNAL_BUS_OK) {
		bootstrap[GO4_BUS_BOOTSTRAP_DIRECT_C7] = *backend_c7;
		if (direct_selected(*backend_c7)) {
			bootstrap[GO4_BUS_BOOTSTRAP_STATE] = GO4_BUS_BOOTSTRAP_FAST;
			goto finished;
		}
		result = GO4_INTERNAL_BUS_UNSUPPORTED_BACKEND;
	}
	uint8_t value = 0;
	uint32_t status = go4_internal_bootstrap_read(ctx, BACKEND_REGISTER, &value,
						      &bootstrap[GO4_BUS_BOOTSTRAP_READ_ATTEMPTS]);
	bootstrap[GO4_BUS_BOOTSTRAP_READ_STATUS] = status;
	if (status == GO4_BOOTSTRAP_OK) {
		bootstrap[GO4_BUS_BOOTSTRAP_BEFORE_C7] = value;
		value = (value | BACKEND_INTERNAL_ENABLED) & ~BACKEND_ALTERNATE_SELECTED;
		status = go4_internal_bootstrap_write(ctx, BACKEND_REGISTER, value,
						      &bootstrap[GO4_BUS_BOOTSTRAP_WRITE_ATTEMPTS]);
		bootstrap[GO4_BUS_BOOTSTRAP_WRITE_STATUS] = status;
		if (status == GO4_BOOTSTRAP_OK || status == GO4_BOOTSTRAP_STOP_FAILED) {
			/* A successful transport is not proof that the backend selected
			 * direct access. A failed STOP also cannot prove that the data
			 * write was rejected: C7 switches the access route itself.
			 * Native 0x497D8 logs initializer errors, publishes address 0x34,
			 * and permits subsequent direct reads. Reconcile only this
			 * cleanup result through direct readback, retaining the error
			 * above; other failed write classes remain terminal. */
			if (confirm_backend(ctx, status, backend_c7)) {
				bootstrap[GO4_BUS_BOOTSTRAP_STATE] = GO4_BUS_BOOTSTRAP_DONE;
				result = GO4_INTERNAL_BUS_OK;
				goto finished;
			}
		}
	}
failed:
	bootstrap[GO4_BUS_BOOTSTRAP_STATE] = GO4_BUS_BOOTSTRAP_FAILED;
finished:
	capture_hardware(ctx, GO4_BUS_BOOTSTRAP_AFTER_CLOCK_SOURCE);
	return result;
}
