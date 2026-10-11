/* SPDX-License-Identifier: Apache-2.0 */
#ifndef DRIVERS_GPIO_ZGMICRO_GPIO_WS9652A_H_
#define DRIVERS_GPIO_ZGMICRO_GPIO_WS9652A_H_

#include <stdint.h>
#include <zephyr/sys/sys_io.h>

/* Shared by the GPIO and pinctrl drivers while holding ws9652a_pad_lock.
 * Per-pin aliases update only selected output latches without changing pad
 * routing or direction, or reading/modifying the whole output register.
 */
static inline void ws9652a_gpio_latches_write(uintptr_t aliases, uint32_t mask, uint32_t values)
{
	while (mask) {
		unsigned int pin = (unsigned int)__builtin_ctz(mask);

		sys_write32((values >> pin) & 1u, aliases + pin * sizeof(uint32_t));
		mask &= mask - 1u;
	}
}

#endif /* DRIVERS_GPIO_ZGMICRO_GPIO_WS9652A_H_ */
