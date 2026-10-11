/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZEPHYR_DT_BINDINGS_PINCTRL_WS9652A_H_
#define ZEPHYR_DT_BINDINGS_PINCTRL_WS9652A_H_

/* Bits 0..4: pin; bit 5: bank; bits 8..31: function.
 * Bits 6..7 are reserved. Banks 0/1 support GPIO; bank0 pins 7/8 also support I2C0.
 * Invalid bank/pin arguments produce reserved bits and are rejected.
 */
#define WS9652A_FUNC_GPIO 0
#define WS9652A_FUNC_I2C0 1
#define WS9652A_PINMUX(bank, pin, function)                                                        \
	(((bank) < 0 || (bank) > 1 || (pin) < 0 || (pin) > 31) ? 0xc0                              \
	 : ((function) < 0 || (function) > 0xffffff)                                               \
		 ? 0xc0                                                                            \
		 : (((function) << 8) | ((bank) << 5) | (pin)))

/* Driver configuration flags, separate from the pinmux encoding. */
#define WS9652A_PINCFG_INPUT          (1U << 0)
#define WS9652A_PINCFG_OUTPUT         (1U << 1)
#define WS9652A_PINCFG_OUTPUT_LOW     (1U << 2)
#define WS9652A_PINCFG_OUTPUT_HIGH    (1U << 3)
#define WS9652A_PINCFG_BIAS_DISABLE   (1U << 4)
#define WS9652A_PINCFG_BIAS_PULL_DOWN (1U << 5)

#endif
