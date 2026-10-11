/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_GO4_IPC_ARENA_H
#define CWBEAT_GO4_IPC_ARENA_H
#include <stdbool.h>
#include <stdint.h>
struct peer_media_state {
	uint8_t connection, playing, auxiliary;
};
enum ipc_bt_readiness {
	IPC_BT_READINESS_UNAVAILABLE,
	IPC_BT_READINESS_INITIALIZED,
	IPC_BT_READINESS_AWAKE,
	IPC_BT_READINESS_ACTIVE,
};
bool ipc_arena_init(void);
uint8_t ipc_arena_run_flags(void);
struct peer_media_state ipc_arena_media_state(void);
uint8_t ipc_arena_bt_readiness(void);
bool ipc_arena_bt_ready(void);
#endif
