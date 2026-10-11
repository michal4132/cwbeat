/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_BLUETOOTH_H
#define CWBEAT_BLUETOOTH_H
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/zbus/zbus.h>
enum bluetooth_command_kind {
	BLUETOOTH_ENABLE,
	BLUETOOTH_DISABLE,
	BLUETOOTH_PLAY_PAUSE,
	BLUETOOTH_NEXT,
	BLUETOOTH_PREVIOUS,
	BLUETOOTH_VOLUME_UP,
	BLUETOOTH_VOLUME_DOWN,
	BLUETOOTH_DISCOVER,
	BLUETOOTH_PAIR,
	BLUETOOTH_LINK
};
struct bluetooth_command {
	uint32_t id;
	enum bluetooth_command_kind command;
};
struct bluetooth_result {
	uint32_t id, session;
	enum bluetooth_command_kind command;
	int status;
};
enum bluetooth_phase {
	BLUETOOTH_DISABLED,
	BLUETOOTH_PREPARING,
	BLUETOOTH_ENABLING,
	BLUETOOTH_AWAKE,
	BLUETOOTH_STOPPING,
	BLUETOOTH_STOPPED,
	BLUETOOTH_FAILED
};
struct bluetooth_state {
	uint32_t session;
	enum bluetooth_phase phase;
	uint8_t readiness, connection, playing, auxiliary;
	bool admission_closed;
	int error;
};
ZBUS_CHAN_DECLARE(bluetooth_commands, bluetooth_results, bluetooth_states);
/* Reserves bounded ingress and terminal delivery before publication.
 * ENABLE completes when stock BT wake is observed. Audio owns its independent
 * mode-4 and codec readiness; neither result proves audible playback. */
int bluetooth_request(enum bluetooth_command_kind command, uint32_t *id);
struct bluetooth_state bluetooth_snapshot(void);
#endif
