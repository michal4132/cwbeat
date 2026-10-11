/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_POWER_PRIVATE_H
#define CWBEAT_POWER_PRIVATE_H
#include <stdbool.h>
#include <cwbeat/power.h>
bool power_autostart_allowed(void);
bool power_hardware_boot_requested(void);
void power_hardware_on(void);
bool power_hardware_console_done(void);
int power_hardware_terminal(enum power_command_kind command);
#endif
