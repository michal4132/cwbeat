/* SPDX-License-Identifier: Apache-2.0 */
#include <cwbeat/battery.h>
#include <zephyr/sys/util.h>
#include <errno.h>

#define BATTERY_ADC_MASK   0x3f
#define BATTERY_TRIM_COUNT 2

int cwbeat_battery_read(const struct device *adc, struct cwbeat_battery_sample *out,
			struct cwbeat_battery_diagnostic *diagnostic)
{
	struct cwbeat_battery_diagnostic trace = {
		.conversion.cleanup_status = WS9652A_ADC_UNOBSERVED,
	};
	struct cwbeat_battery_sample sample;
	unsigned sum = 0;
	uint8_t low = UINT8_MAX, high = 0;
	int error = -EINVAL;

	if (!out) {
		goto done;
	}
	for (unsigned i = 0; i < CWBEAT_BATTERY_SAMPLES; ++i) {
		// TODO(mb): use zephyr adc/sensor api
		error = ws9652a_adc_read_raw(adc, &sample.raw[i], &trace.conversion);
		if (error) {
			goto done;
		}
		++trace.completed;
		sum += sample.raw[i];
		low = MIN(low, sample.raw[i]);
		high = MAX(high, sample.raw[i]);
	}
	/* Stock 0x4237C: trim whole bytes, average five, THEN mask to six bits. */
	sample.adc = ((sum - low - high) / (CWBEAT_BATTERY_SAMPLES - BATTERY_TRIM_COUNT)) &
		     BATTERY_ADC_MASK;
	*out = sample;
done:
	if (diagnostic) {
		*diagnostic = trace;
	}
	return error;
}
