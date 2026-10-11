#ifndef GO4_POWER_OFF_H
#define GO4_POWER_OFF_H

#include <stdbool.h>
/* This boot's sampled Power input. No state is retained across reset. */
bool power_boot_requested(void);
/* Call only after console drain, BT shutdown and peer/audio safe barriers.
 * Returns false before any mutation if the delay clock is unavailable.
 * Otherwise performs the stock platform shutdown and never returns. */
bool power_off_request(void);

#endif
