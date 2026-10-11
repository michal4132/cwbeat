/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZEPHYR_SOC_WS9652A_PINCTRL_SOC_H_
#define ZEPHYR_SOC_WS9652A_PINCTRL_SOC_H_
#include <zephyr/types.h>
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/pinctrl/ws9652a-pinctrl.h>

typedef struct {
	uint32_t pinmux;
	uint32_t config;
} pinctrl_soc_pin_t;

#define Z_PINCTRL_WS9652A_CONFIG(node_id)                                                          \
	((DT_PROP(node_id, input_enable) ? WS9652A_PINCFG_INPUT : 0) |                             \
	 (DT_PROP(node_id, output_enable) ? WS9652A_PINCFG_OUTPUT : 0) |                           \
	 (DT_PROP(node_id, output_low) ? WS9652A_PINCFG_OUTPUT_LOW : 0) |                          \
	 (DT_PROP(node_id, output_high) ? WS9652A_PINCFG_OUTPUT_HIGH : 0) |                        \
	 (DT_PROP(node_id, bias_disable) ? WS9652A_PINCFG_BIAS_DISABLE : 0) |                      \
	 (DT_PROP(node_id, bias_pull_down) ? WS9652A_PINCFG_BIAS_PULL_DOWN : 0))
#define Z_PINCTRL_WS9652A_PIN_INIT(node_id, prop, idx)                                             \
	{.pinmux = DT_PROP_BY_IDX(node_id, prop, idx), .config = Z_PINCTRL_WS9652A_CONFIG(node_id)}
#define Z_PINCTRL_WS9652A_GROUP_INIT(node_id)                                                      \
	DT_FOREACH_PROP_ELEM_SEP(node_id, pinmux, Z_PINCTRL_WS9652A_PIN_INIT, (,)),
#define Z_PINCTRL_WS9652A_STATE_INIT(node_id, prop, idx)                                           \
	DT_FOREACH_CHILD(DT_PHANDLE_BY_IDX(node_id, prop, idx), Z_PINCTRL_WS9652A_GROUP_INIT)
#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)                                                   \
	{DT_FOREACH_PROP_ELEM(node_id, prop, Z_PINCTRL_WS9652A_STATE_INIT)}
#endif
