/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_AUDIO_PRIVATE_H
#define CWBEAT_AUDIO_PRIVATE_H
#include <stdbool.h>
int audio_hardware_prepare(void);
int audio_hardware_write(unsigned pin, bool high);
#endif
