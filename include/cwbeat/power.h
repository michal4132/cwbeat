/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_POWER_H
#define CWBEAT_POWER_H
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/zbus/zbus.h>
enum power_command_kind {
	POWER_ON,
	POWER_OFF,
	POWER_STOP,
	POWER_RESET,
	POWER_RECOVERY
};
struct power_command {
	uint32_t id;
	enum power_command_kind command;
};
struct power_result {
	uint32_t id;
	enum power_command_kind command;
	int status;
};
struct power_state {
	enum power_command_kind intent;
	/* Runtime off intent, initialized from this boot's physical Power input. */
	bool off_pending, transitioning;
	int error;
};
ZBUS_CHAN_DECLARE(power_commands, power_results, power_states);
/* Terminal intent cancels queued ON requests and prohibits ON until reset.
 * A boot waiting for Power may accept its first explicit ON. */
int power_request(enum power_command_kind command, uint32_t *id);
struct power_state power_snapshot(void);
#endif
