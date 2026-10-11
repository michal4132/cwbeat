/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZEPHYR_DRIVERS_ADC_WS9652A_H
#define ZEPHYR_DRIVERS_ADC_WS9652A_H

#include <zephyr/device.h>
#include <stdbool.h>
#include <stdint.h>

#define WS9652A_ADC_BATTERY_CHANNEL 0
#define WS9652A_ADC_RAW_BITS        8
#define WS9652A_ADC_UNOBSERVED      UINT32_MAX

struct ws9652a_adc_diagnostic {
	uint32_t bus_status;
	/* Last disarm write's bus status, or UNOBSERVED if none was attempted. */
	uint32_t cleanup_status;
	uint16_t prerequisite;
	uint8_t index, value;
	bool write, blocked;
};

/* One class-0x14 battery conversion, preserving all bits of the result byte.
 * Thread context with interrupts enabled, after successful PMU preparation.
 * The shared parent transaction lock spans conversion polling and cleanup;
 * it does not exclude BT/DSP. Caller owns TOP's battery conversion pipeline.
 * Output is unchanged on failure; optional diagnostic is always populated.
 * A failed write blocks further conversion transfers until reset and retains
 * the original failure diagnostic. An inherited nonzero 05 trigger is disarmed
 * once and returns -EBUSY; this operation never retries that conversion.
 *
 * The standard ADC API reads the same conversion as one uint16_t sample,
 * zero-extending the register byte, with channel 0 and sequence resolution 8.
 * This describes the raw representation, not a known effective resolution.
 * Channel gain 1, INTERNAL reference and DEFAULT acquisition select the fixed,
 * inherited analog configuration; they do not establish its voltage scale.
 * No reference voltage is advertised. Oversampling, calibration, extra samples,
 * callbacks, differential inputs and asynchronous reads are unsupported.
 */
int ws9652a_adc_read_raw(const struct device *dev, uint8_t *out,
			 struct ws9652a_adc_diagnostic *diagnostic);

#endif
