/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_STOCK_PEER_H
#define CWBEAT_STOCK_PEER_H
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/zbus/zbus.h>
enum stock_peer_owner {
	STOCK_PEER_OWNER_BLUETOOTH,
	STOCK_PEER_OWNER_AUDIO
};
struct stock_peer_token {
	uint32_t session, id;
	enum stock_peer_owner owner;
};
enum stock_peer_operation {
	STOCK_PEER_PREPARE,
	STOCK_PEER_RELEASE,
	STOCK_PEER_HOLD,
	STOCK_PEER_WAKE,
	STOCK_PEER_SHUTDOWN,
	STOCK_PEER_RAW_ACTIVE,
	STOCK_PEER_DSP_PROFILE,
	STOCK_PEER_FEEDBACK_DISABLE,
	STOCK_PEER_PLAY,
	STOCK_PEER_PAUSE,
	STOCK_PEER_NEXT,
	STOCK_PEER_PREVIOUS,
	STOCK_PEER_VOLUME_UP,
	STOCK_PEER_VOLUME_DOWN,
	STOCK_PEER_DISCOVER,
	STOCK_PEER_PAIR,
	STOCK_PEER_LINK
};
enum stock_peer_profile {
	STOCK_PEER_PROFILE_DEFAULT,
	STOCK_PEER_PROFILE_RUN_OVERRIDE
};
struct stock_peer_request {
	struct stock_peer_token token;
	enum stock_peer_operation operation;
	union {
		enum stock_peer_profile profile;
	} parameters;
};
struct stock_peer_result {
	struct stock_peer_token token;
	enum stock_peer_operation operation;
	int status;
};
enum stock_peer_phase {
	STOCK_PEER_IDLE,
	STOCK_PEER_PREPARED,
	STOCK_PEER_RELEASED,
	STOCK_PEER_HELD,
	STOCK_PEER_FAILED
};
struct stock_peer_state {
	uint32_t session;
	enum stock_peer_phase phase;
	uint32_t board_type;
	uint8_t readiness, run_flags, connection, playing, auxiliary;
	bool admission_closed;
	int error;
};
enum stock_peer_callback_kind {
	STOCK_PEER_POWER_MODE,
	STOCK_PEER_CODEC_CAPABILITIES,
	STOCK_PEER_CODEC_INIT,
	STOCK_PEER_CODEC_SHUTDOWN,
	STOCK_PEER_CODEC_ENABLE
};
struct stock_peer_callback {
	uint32_t session, token;
	enum stock_peer_callback_kind kind;
	uint8_t mode;
};
struct stock_peer_callback_response {
	uint32_t session, token;
	int status;
	bool capabilities_ready;
};
ZBUS_CHAN_DECLARE(stock_peer_requests, stock_peer_bt_results, stock_peer_audio_results,
		  stock_peer_states, stock_peer_callbacks, stock_peer_callback_responses);
/* Submission reserves both ingress and terminal storage. Success means copied local delivery. */
int stock_peer_request(const struct stock_peer_request *request);
int stock_peer_callback_respond(const struct stock_peer_callback_response *response);
/* Dispatch barrier. Keeps callback and shutdown control traffic admitted. */
int stock_peer_close_admission(uint32_t session);
/* Last owner snapshot; state notifications may coalesce. */
struct stock_peer_state stock_peer_snapshot(void);
#endif
