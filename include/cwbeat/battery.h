/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_BATTERY_H
#define CWBEAT_BATTERY_H

#include <zephyr/drivers/adc/ws9652a.h>

#define CWBEAT_BATTERY_SAMPLES 7

struct cwbeat_battery_sample {
	/* Stock trimmed mean, masked to six bits AFTER averaging. Not mV or %. */
	uint8_t adc;
	uint8_t raw[CWBEAT_BATTERY_SAMPLES];
};

struct cwbeat_battery_diagnostic {
	struct ws9652a_adc_diagnostic conversion;
	/* Successful conversions in this call; zero for an already blocked ADC. */
	uint8_t completed;
};

/* Run the stock seven-sample filter using the WS9652A ADC child device.
 * Each conversion takes its own parent transaction lock. On failure the output
 * is unchanged and the diagnostic reports the current batch's completed count.
 * The ADC retains any blocking conversion failure separately until reset.
 */
int cwbeat_battery_read(const struct device *adc, struct cwbeat_battery_sample *out,
			struct cwbeat_battery_diagnostic *diagnostic);

#endif
