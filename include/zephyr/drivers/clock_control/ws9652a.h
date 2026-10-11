/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZEPHYR_INCLUDE_DRIVERS_CLOCK_CONTROL_WS9652A_H_
#define ZEPHYR_INCLUDE_DRIVERS_CLOCK_CONTROL_WS9652A_H_

#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/clock/ws9652a-clock.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ws9652a_clock_registers {
	uint32_t source, select, divider;
};

/* Read raw fields in source/select/divider order without masking or retrying.
 * These are sequential observations, not an atomic clock-tree snapshot.
 * Returns -EINVAL for a null device/output and leaves output unchanged. */
int ws9652a_clock_control_get_registers(const struct device *dev,
				      struct ws9652a_clock_registers *out);

/* Read the clkctrl provider before device initialization. Uses only constant
 * devicetree addresses/frequency, read-only tables, stack locals and MMIO reads.
 * Like clock_control_get_rate(), makes at most three sampling attempts total.
 * Double sampling rejects detected changes but does not exclude peer writes.
 * Returns -EINVAL for an invalid ID/null output, -EAGAIN for unstable
 * observations, -ERANGE for reserved encodings, or -ENODEV without a provider.
 * Output is unchanged on error. IDs are WS9652A_CLOCK_CORE and WS9652A_CLOCK_BUS;
 * runtime clock_control_get_rate() takes the ID cast to clock_control_subsys_t.
 * No operation changes clock gates, source selection or dividers. */
int ws9652a_clock_control_get_rate_early(uint32_t clock, uint32_t *rate);

#ifdef __cplusplus
}
#endif

#endif
