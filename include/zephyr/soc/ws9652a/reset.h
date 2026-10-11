/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WS9652A_RESET_H
#define WS9652A_RESET_H
#include <stdint.h>
/* Interrupts must be disabled. No BSS, devices, locks, ROM calls or logging. */
void ws9652a_usb_detach_emergency(void);
__attribute__((noreturn)) void ws9652a_reset_immediate(void);
/* Called before BSS initialization, after validating a bootloader profile. */
__attribute__((noreturn)) void ws9652a_boot_handoff(uintptr_t vectors, uintptr_t stack,
						    uintptr_t entry, uint32_t argument);
#endif
