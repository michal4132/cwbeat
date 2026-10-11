/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WS9652A_PINCTRL_H
#define WS9652A_PINCTRL_H
#include <stdint.h>
#include <zephyr/dt-bindings/pinctrl/ws9652a-pinctrl.h>
#include <zephyr/kernel.h>
/* All normal GPIO/pinctrl MMIO transactions share this lock. Configuration
 * helpers acquire it internally; callers must not hold it across those calls. */
extern struct k_spinlock ws9652a_pad_lock;
/* GPIO input/output profiles default to disabled bias. BIAS_PULL_DOWN selects
 * the 22 kOhm pull-down. A routing-only profile leaves bias unchanged unless
 * BIAS_DISABLE or BIAS_PULL_DOWN is explicit. GPIO0.0 output profiles preserve
 * its separate input bias and reject explicit bias flags. */
int ws9652a_pinctrl_gpio_configure(unsigned bank, uint32_t mask, uint32_t config);
#endif
