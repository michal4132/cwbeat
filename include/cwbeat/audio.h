/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_AUDIO_H
#define CWBEAT_AUDIO_H
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/zbus/zbus.h>
enum audio_operation {
	AUDIO_PREPARE,
	AUDIO_QUIESCE
};
struct audio_command {
	uint32_t session, id;
	enum audio_operation operation;
};
struct audio_result {
	uint32_t session, id;
	enum audio_operation operation;
	int status;
};
struct audio_state {
	uint32_t session;
	bool controls_ready, mode3_ready, mode4_ready, codec_initialized;
	/* Includes raw active-mode ACK only when audio actually requested it. */
	bool ready;
	bool raises_revoked;
	/* Held-peer validated boundary: GPIO16 low, no future raises/work.
	 * GPIO12 retains its latch unless native command4 completed both lows. */
	bool quiescent, callbacks_settled;
	int error;
};
ZBUS_CHAN_DECLARE(audio_commands, audio_results, audio_states);
int audio_request(const struct audio_command *command);
/* Serializes with the final permission check and GPIO write. */
int audio_revoke_raises(uint32_t session);
struct audio_state audio_snapshot(void);
#endif
