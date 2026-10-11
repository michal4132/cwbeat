/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_UI_PRIVATE_H
#define CWBEAT_UI_PRIVATE_H
#include "gestures.h"
/* Input history boundary also used by standalone fixtures. */
void ui_input_event(enum cwbeat_button button, bool down, uint32_t when);
int ui_hardware_led(unsigned channel, bool on);
#endif
