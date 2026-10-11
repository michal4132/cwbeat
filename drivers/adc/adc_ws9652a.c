/* SPDX-License-Identifier: Apache-2.0 */
/* Stock TOP 0x46B9C + prerequisite 0x46794, class 0x14 only. */
#define DT_DRV_COMPAT zgmicro_ws9652a_adc

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/ws9652a.h>
#include <zephyr/drivers/mfd/ws9652a.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(adc_ws9652a, CONFIG_ADC_WS9652A_LOG_LEVEL);

struct adc_ws9652a_config {
	const struct device *pmu;
};

struct adc_ws9652a_data {
	struct ws9652a_adc_diagnostic failure;
};

struct adc_ws9652a_context {
	const struct device *pmu;
	struct adc_ws9652a_data *data;
};

enum {
	BATTERY_CONTROL = 0x05,
	BATTERY_RESULT = 0x06,
	PREREQUISITE_ENABLE_08 = 0x08,
	PREREQUISITE_ENABLE_09 = 0x09,
	PREREQUISITE_CONTROL = 0x17,
	PREREQUISITE_STATUS = 0x18,
	PREREQUISITE_LOW = 0x19,
	PREREQUISITE_HIGH = 0x1a,
	BATTERY_STATUS = 0xc0,
	BACKEND_C7 = 0xc7,
	IDENTITY_F4 = 0xf4,
	IDENTITY_F5 = 0xf5,
	ID_F5_NEEDS_F4 = 1,
	ID_F4_CLASS_14 = 0xbb,
	ID_F5_CLASS_14_FIRST = 2,
	ID_F5_CLASS_14_LAST = 4,
	ENABLE_08_MASK = BIT(5),
	ENABLE_09_MASK = BIT(0),
	PREREQUISITE_READY = BIT(0),
	BATTERY_READY = BIT(1),
	TRIGGER_START = 1,
	TRIGGER_STOP = 0,
	PREREQUISITE_POLLS = 10,
	PREREQUISITE_WAIT_US = 300,
	PREREQUISITE_MINIMUM = 436,
	BATTERY_POLLS = 30,
	BATTERY_WAIT_US = 600,
	BYTE_BITS = 8,
};

static int read_byte(struct adc_ws9652a_context *ctx, uint8_t index, uint8_t *out,
		     struct ws9652a_adc_diagnostic *trace)
{
	trace->index = index;
	trace->write = false;
	trace->value = UINT8_MAX;
	trace->bus_status = ws9652a_pmu_read_locked(ctx->pmu, index, out);
	if (!trace->bus_status) {
		trace->value = *out;
	}
	return ws9652a_pmu_bus_error(trace->bus_status);
}

static int write_byte(struct adc_ws9652a_context *ctx, uint8_t index, uint8_t value,
		      struct ws9652a_adc_diagnostic *trace)
{
	trace->index = index;
	trace->write = true;
	trace->value = value;
	trace->bus_status = ws9652a_pmu_write_locked(ctx->pmu, index, value);
	trace->blocked |= trace->bus_status != 0;
	return ws9652a_pmu_bus_error(trace->bus_status);
}

static int disarm(struct adc_ws9652a_context *ctx, uint8_t index, int error,
		  struct ws9652a_adc_diagnostic *trace)
{
	/* A failed start may still have reached the PMU. Attempt stop once, keep
	 * the first failure, and never turn failed cleanup into a valid sample. */
	trace->cleanup_status = ws9652a_pmu_write_locked(ctx->pmu, index, TRIGGER_STOP);
	if (trace->cleanup_status) {
		trace->blocked = true;
		if (!error) {
			trace->index = index;
			trace->value = TRIGGER_STOP;
			trace->write = true;
			trace->bus_status = trace->cleanup_status;
			error = ws9652a_pmu_bus_error(trace->cleanup_status);
		}
	}
	return error;
}

static int enable_bit(struct adc_ws9652a_context *ctx, uint8_t index, uint8_t mask,
		      struct ws9652a_adc_diagnostic *trace)
{
	uint8_t value;
	int error = read_byte(ctx, index, &value, trace);
	if (!error && !(value & mask)) {
		error = write_byte(ctx, index, value | mask, trace);
	}
	return error;
}

static int wait_ready(struct adc_ws9652a_context *ctx, uint8_t index, uint8_t mask,
		      unsigned attempts, unsigned wait_us, struct ws9652a_adc_diagnostic *trace)
{
	for (unsigned attempt = 0; attempt < attempts; ++attempt) {
		/* No retained timer0 calls. The Zephyr counter remains the timebase. */
		k_usleep(wait_us);
		uint8_t status;
		int error = read_byte(ctx, index, &status, trace);
		if (error || (status & mask)) {
			return error;
		}
	}
	return -ETIMEDOUT;
}

static int prerequisite(struct adc_ws9652a_context *ctx, struct ws9652a_adc_diagnostic *trace)
{
	int error = enable_bit(ctx, PREREQUISITE_ENABLE_08, ENABLE_08_MASK, trace);
	if (!error) {
		error = enable_bit(ctx, PREREQUISITE_ENABLE_09, ENABLE_09_MASK, trace);
	}
	if (error) {
		return error;
	}
	error = write_byte(ctx, PREREQUISITE_CONTROL, TRIGGER_START, trace);
	if (!error) {
		error = wait_ready(ctx, PREREQUISITE_STATUS, PREREQUISITE_READY, PREREQUISITE_POLLS,
				   PREREQUISITE_WAIT_US, trace);
	}
	uint8_t high, low;
	if (!error) {
		error = read_byte(ctx, PREREQUISITE_HIGH, &high, trace);
	}
	if (!error) {
		error = read_byte(ctx, PREREQUISITE_LOW, &low, trace);
	}
	if (!error) {
		trace->prerequisite = ((uint16_t)high << BYTE_BITS) | low;
	}
	error = disarm(ctx, PREREQUISITE_CONTROL, error, trace);
	if (!error && trace->prerequisite < PREREQUISITE_MINIMUM) {
		/* Stock arms 05 here and returns 0x431. Do not arm it on this path. */
		error = -ERANGE;
	}
	return error;
}

static int verify_identity(struct adc_ws9652a_context *ctx, struct ws9652a_adc_diagnostic *trace)
{
	uint8_t value;
	trace->index = BACKEND_C7;
	trace->write = false;
	trace->value = UINT8_MAX;
	trace->bus_status = ws9652a_pmu_verify_backend_locked(ctx->pmu, &value);
	if (!trace->bus_status || ws9652a_pmu_bus_error(trace->bus_status) == -ENOTSUP) {
		trace->value = value;
	}
	if (trace->bus_status) {
		return ws9652a_pmu_bus_error(trace->bus_status);
	}
	int error = read_byte(ctx, IDENTITY_F5, &value, trace);
	if (error) {
		return error;
	}
	if (value == ID_F5_NEEDS_F4) {
		error = read_byte(ctx, IDENTITY_F4, &value, trace);
		return error ? error : value == ID_F4_CLASS_14 ? 0 : -ENOTSUP;
	}
	return value >= ID_F5_CLASS_14_FIRST && value <= ID_F5_CLASS_14_LAST ? 0 : -ENOTSUP;
}

/* Retain a call boundary for the native stock-instruction conversion oracle. */
static __noinline int adc_ws9652a_sample_locked(struct adc_ws9652a_context *ctx, uint8_t *out,
						struct ws9652a_adc_diagnostic *trace)
{
	if (ctx->data->failure.blocked) {
		*trace = ctx->data->failure;
		return -EIO;
	}
	trace->cleanup_status = WS9652A_ADC_UNOBSERVED;
	trace->prerequisite = 0;
	int error = verify_identity(ctx, trace);
	if (!error) {
		error = prerequisite(ctx, trace);
	}
	if (error) {
		goto done;
	}
	uint8_t value;
	error = read_byte(ctx, BATTERY_CONTROL, &value, trace);
	if (error) {
		/* No battery start was attempted, so don't clear an unknown control. */
		goto done;
	}
	if (value != TRIGGER_STOP) {
		/* Stock returns 0x430 and clears 05, including the trigger armed by
		 * the final preparation policy. The caller must request a new read. */
		error = disarm(ctx, BATTERY_CONTROL, -EBUSY, trace);
		goto done;
	}
	error = write_byte(ctx, BATTERY_CONTROL, TRIGGER_START, trace);
	if (!error) {
		error = wait_ready(ctx, BATTERY_STATUS, BATTERY_READY, BATTERY_POLLS,
				   BATTERY_WAIT_US, trace);
	}
	if (!error) {
		error = read_byte(ctx, BATTERY_RESULT, &value, trace);
	}
	error = disarm(ctx, BATTERY_CONTROL, error, trace);
	if (!error) {
		*out = value;
	}
done:
	if (trace->blocked) {
		ctx->data->failure = *trace;
	}
	return error;
}

struct adc_ws9652a_request {
	struct adc_ws9652a_context context;
	uint8_t *out;
	struct ws9652a_adc_diagnostic *trace;
	bool attempted, was_blocked;
};

static int adc_ws9652a_convert(const struct device *pmu, void *context)
{
	struct adc_ws9652a_request *request = context;

	request->context.pmu = pmu;
	request->attempted = true;
	request->was_blocked = request->context.data->failure.blocked;
	return adc_ws9652a_sample_locked(&request->context, request->out, request->trace);
}

static void adc_ws9652a_log_result(const struct device *dev,
				 const struct adc_ws9652a_request *request, int error)
{
	/* Report after the PMU transaction, never from a byte transfer or an
	 * invalid caller context. The child has no controller or CPU dependency. */
	if (!IS_ENABLED(CONFIG_LOG) || k_is_pre_kernel() || k_is_in_isr()) {
		return;
	}
	const struct ws9652a_adc_diagnostic *trace = request->trace;
	if (!request->attempted || request->was_blocked) {
		LOG_DBG("%d: %s: conversion refused", error, dev->name);
	} else if (trace->cleanup_status != WS9652A_ADC_UNOBSERVED && trace->cleanup_status) {
		LOG_ERR("%d: %s: conversion cleanup failed: bus=0x%x cleanup=0x%x",
			error, dev->name, trace->bus_status, trace->cleanup_status);
	} else if (!trace->blocked &&
		   (error == -EBUSY || error == -EAGAIN || error == -ENOTSUP)) {
		LOG_DBG("%d: %s: conversion refused: register=0x%x bus=0x%x",
			error, dev->name, trace->index, trace->bus_status);
	} else if (error) {
		LOG_ERR("%d: %s: conversion failed: register=0x%x bus=0x%x blocked=%u",
			error, dev->name, trace->index, trace->bus_status, trace->blocked);
	} else {
		LOG_DBG("%s: conversion complete: raw=0x%x", dev->name, *request->out);
	}
}

int ws9652a_adc_read_raw(const struct device *dev, uint8_t *out,
			 struct ws9652a_adc_diagnostic *diagnostic)
{
	struct ws9652a_adc_diagnostic trace = {
		.cleanup_status = WS9652A_ADC_UNOBSERVED,
	};
	int error = -EINVAL;

	if (out) {
		const struct adc_ws9652a_config *config = dev->config;
		struct adc_ws9652a_request request = {
			.context.data = dev->data,
			.out = out,
			.trace = &trace,
		};

		error = ws9652a_pmu_transaction(config->pmu, adc_ws9652a_convert, &request);
		adc_ws9652a_log_result(dev, &request, error);
	}
	if (diagnostic) {
		*diagnostic = trace;
	}
	return error;
}

static int adc_ws9652a_channel_setup(const struct device *dev,
				     const struct adc_channel_cfg *channel)
{
	ARG_UNUSED(dev);
	if (!channel || channel->channel_id != WS9652A_ADC_BATTERY_CHANNEL) {
		return -EINVAL;
	}
	if (channel->differential || channel->gain != ADC_GAIN_1 ||
	    channel->reference != ADC_REF_INTERNAL ||
	    channel->acquisition_time != ADC_ACQ_TIME_DEFAULT) {
		return -ENOTSUP;
	}
#ifdef CONFIG_ADC_CONFIGURABLE_INPUTS
	if (channel->input_positive || channel->input_negative) {
		return -ENOTSUP;
	}
#endif
#ifdef CONFIG_ADC_CONFIGURABLE_EXCITATION_CURRENT_SOURCE_PIN
	if (channel->current_source_pin_set) {
		return -ENOTSUP;
	}
#endif
#ifdef CONFIG_ADC_CONFIGURABLE_VBIAS_PIN
	if (channel->vbias_pins) {
		return -ENOTSUP;
	}
#endif
	return 0;
}

static int adc_ws9652a_read(const struct device *dev, const struct adc_sequence *sequence)
{
	if (!sequence || !sequence->buffer ||
	    sequence->channels != BIT(WS9652A_ADC_BATTERY_CHANNEL)) {
		return -EINVAL;
	}
	if (sequence->resolution != WS9652A_ADC_RAW_BITS || sequence->oversampling ||
	    sequence->calibrate || sequence->options) {
		return -ENOTSUP;
	}
	if (sequence->buffer_size < sizeof(uint16_t)) {
		return -ENOMEM;
	}
	uint8_t raw;
	int error = ws9652a_adc_read_raw(dev, &raw, NULL);

	if (!error) {
		uint16_t sample = raw;

		memcpy(sequence->buffer, &sample, sizeof(sample));
	}
	return error;
}

#ifdef CONFIG_ADC_ASYNC
static int adc_ws9652a_read_async(const struct device *dev, const struct adc_sequence *sequence,
				  struct k_poll_signal *signal)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sequence);
	ARG_UNUSED(signal);
	return -ENOTSUP;
}
#endif

static int adc_ws9652a_ref_get(const struct device *dev, enum adc_reference reference,
			       uint16_t *voltage_mv)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(voltage_mv);
	return reference == ADC_REF_INTERNAL ? -ENODATA : -ENOTSUP;
}

static int adc_ws9652a_init(const struct device *dev)
{
	const struct adc_ws9652a_config *config = dev->config;

	if (!device_is_ready(config->pmu)) {
		LOG_ERR("%d: %s: PMU %s not ready", -ENODEV, dev->name, config->pmu->name);
		return -ENODEV;
	}
	LOG_DBG("%s: initialized", dev->name);
	return 0;
}

static DEVICE_API(adc, adc_ws9652a_api) = {
	.channel_setup = adc_ws9652a_channel_setup,
	.read = adc_ws9652a_read,
	.ref_internal = 0,
	.ref_get = adc_ws9652a_ref_get,
#ifdef CONFIG_ADC_ASYNC
	.read_async = adc_ws9652a_read_async,
#endif
};

#define ADC_WS9652A_DEFINE(inst)                                                                   \
	static const struct adc_ws9652a_config adc_config_##inst = {                               \
		.pmu = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
	};                                                                                         \
	static struct adc_ws9652a_data adc_data_##inst;                                            \
	DEVICE_DT_INST_DEFINE(inst, adc_ws9652a_init, NULL, &adc_data_##inst, &adc_config_##inst,  \
			      POST_KERNEL, CONFIG_ADC_WS9652A_INIT_PRIORITY, &adc_ws9652a_api);

DT_INST_FOREACH_STATUS_OKAY(ADC_WS9652A_DEFINE)
