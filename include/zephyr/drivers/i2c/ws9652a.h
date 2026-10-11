/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZEPHYR_INCLUDE_DRIVERS_I2C_WS9652A_H_
#define ZEPHYR_INCLUDE_DRIVERS_I2C_WS9652A_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ws9652a_i2c_phase {
	WS9652A_I2C_PHASE_NONE,
	WS9652A_I2C_PHASE_PREFLIGHT,
	WS9652A_I2C_PHASE_ADDRESS,
	WS9652A_I2C_PHASE_DATA,
	WS9652A_I2C_PHASE_STOP,
	WS9652A_I2C_PHASE_COMPLETE,
};

struct ws9652a_i2c_result {
	int transfer_error;
	int stop_error;
	uint8_t message_index;
	enum ws9652a_i2c_phase phase;
	/* Completed data bytes across all messages, excluding address bytes.
	 * Transmitted bytes count only after ACK; received bytes after completion.
	 */
	size_t data_completed;
};

/* Same message contract as i2c_transfer(). The optional result is populated
 * even on validation/preflight failure. Returns the transfer error first,
 * otherwise the STOP error. With a transfer error, phase/message_index retain
 * its location even if cleanup also fails. No transfer is automatically retried.
 * Read buffers are published only after complete success, including STOP.
 * Requires thread context and the register-bus peers-held permission.
 */
int ws9652a_i2c_transfer_detailed(const struct device *dev, struct i2c_msg *msgs, uint8_t count,
				  uint16_t address, struct ws9652a_i2c_result *result);

/* These helpers take a WS9652A channel device, not the hardware parent. */
const struct device *ws9652a_i2c_bus(const struct device *dev);

/* Bounded DWT delay using the live core rate. Does not enable/reset the counter.
 * Returns -EIO for an unavailable/stopped counter, -ERANGE if the poll budget
 * would overflow, or a clock/lock error. Thread context is required.
 */
int ws9652a_i2c_delay(const struct device *dev, uint32_t usec);

/* Sequential raw source/select/divider/control/command/status/route/gate reads.
 * Takes the register-bus lock without requiring held peers. This excludes other
 * local transactions, not peer writes; the snapshot is not atomic. Output is
 * unchanged on error. Thread context is required.
 */
int ws9652a_i2c_snapshot(const struct device *dev, uint32_t out[8]);

#ifdef __cplusplus
}
#endif

#endif
