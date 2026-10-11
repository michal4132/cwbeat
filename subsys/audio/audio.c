/* SPDX-License-Identifier: Apache-2.0 */
#include <cwbeat/audio.h>
#include <cwbeat/stock_peer.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include "private.h"

LOG_MODULE_REGISTER(cwbeat_audio, CONFIG_CWBEAT_AUDIO_LOG_LEVEL);

BUILD_ASSERT(CONFIG_ZBUS_CHANNELS_SYS_INIT_PRIORITY < 90,
	     "audio requires initialized zbus channels");
#define COMMAND_DEPTH  2
#define CALLBACK_DEPTH 8
#define CODEC_DEPTH    4
static struct k_spinlock lock;
static struct k_work work;
static struct k_work_delayable deadline;
static struct audio_state state;
static struct stock_peer_state peer;
static struct audio_command commands[COMMAND_DEPTH];
static unsigned command_head, command_count, command_reserved;
static bool publication_inflight;
static struct stock_peer_callback callbacks[CALLBACK_DEPTH];
static unsigned callback_head, callback_count;
static uint32_t last_callback_session, last_callback_token;
static struct stock_peer_result peer_results[2];
static unsigned result_count;
static struct audio_result completion;
static bool completion_pending, command_busy, state_dirty;
static struct stock_peer_callback_response response;
static bool response_pending, mode_pending;
static struct stock_peer_callback mode_callback;
static uint8_t mode_phase;
static int64_t mode_due, codec_due, operation_due, readiness_due;
static enum stock_peer_callback_kind codec_queue[CODEC_DEPTH];
static unsigned codec_head, codec_count;
static bool shutdown_reserved, codec_running, codec_second;
static enum stock_peer_callback_kind codec_operation, response_operation;
static bool response_enqueues;
static struct stock_peer_token outstanding;
static enum stock_peer_operation outstanding_operation;
static bool request_outstanding, raw_sent, raw_done, awake_seen;
static uint32_t next_id;
static const char *failure_reason;

struct audio_log_snapshot {
	struct audio_state state;
	const char *failure_reason;
	uint8_t peer_readiness;
	enum stock_peer_callback_kind codec_operation;
	enum stock_peer_operation peer_operation;
	bool raw_sent, raw_done;
};

/* Called only by the worker, after releasing the state lock. */
static void log_state(const struct audio_log_snapshot *snapshot)
{
	static struct audio_state previous;
	const struct audio_state *current = &snapshot->state;
	bool new_session = current->session != previous.session;

	if (current->controls_ready && (new_session || !previous.controls_ready)) {
		LOG_INF("session=%u controls prepared", current->session);
	}
	if (current->mode3_ready && (new_session || !previous.mode3_ready)) {
		LOG_INF("session=%u mode 3 ready", current->session);
	}
	if (current->mode4_ready && (new_session || !previous.mode4_ready)) {
		LOG_INF("session=%u mode 4 ready", current->session);
	}
	if (current->codec_initialized && (new_session || !previous.codec_initialized)) {
		LOG_INF("session=%u codec initialized", current->session);
	}
	if (current->ready && (new_session || !previous.ready)) {
		LOG_INF("session=%u ready", current->session);
	}
	if (current->quiescent && (new_session || !previous.quiescent)) {
		LOG_INF("session=%u quiescent", current->session);
	}
	if (current->error && (new_session || current->error != previous.error)) {
		LOG_ERR("session=%u %s: error=%d readiness=%u controls=%u "
			"mode3=%u mode4=%u codec=%u raw=%u/%u codec_op=%u peer_op=%u",
			current->session,
			snapshot->failure_reason ? snapshot->failure_reason : "audio failed",
			current->error, snapshot->peer_readiness, current->controls_ready,
			current->mode3_ready, current->mode4_ready, current->codec_initialized,
			snapshot->raw_sent, snapshot->raw_done, snapshot->codec_operation,
			snapshot->peer_operation);
	}
	previous = *current;
}

ZBUS_CHAN_DEFINE(audio_commands, struct audio_command, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(audio_results, struct audio_result, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(audio_states, struct audio_state, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
static void schedule(void)
{
	k_work_submit(&work);
}
static void input(const struct zbus_channel *channel)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (channel == &audio_commands) {
		/* audio_request reserved this slot before publication. */
		if (command_reserved && command_count < COMMAND_DEPTH) {
			commands[(command_head + command_count++) % COMMAND_DEPTH] =
				*(const struct audio_command *)zbus_chan_const_msg(channel);
			--command_reserved;
		}
	} else if (channel == &stock_peer_states) {
		peer = *(const struct stock_peer_state *)zbus_chan_const_msg(channel);
	} else if (channel == &stock_peer_callbacks) {
		const struct stock_peer_callback *callback = zbus_chan_const_msg(channel);
		if (callback->session == state.session && callback->token &&
		    (callback->session != last_callback_session ||
		     callback->token > last_callback_token)) {
			if (callback_count < CALLBACK_DEPTH) {
				callbacks[(callback_head + callback_count++) % CALLBACK_DEPTH] =
					*callback;
				last_callback_session = callback->session;
				last_callback_token = callback->token;
				if (state.raises_revoked) {
					state.callbacks_settled = false;
					state_dirty = true;
				}
			} else {
				state.error = -ENOBUFS;
				failure_reason = "callback queue full";
				state.raises_revoked = true;
				state.ready = false;
				state_dirty = true;
			}
		}
	} else if (channel == &stock_peer_audio_results) {
		const struct stock_peer_result *result = zbus_chan_const_msg(channel);
		if (request_outstanding && result->token.session == outstanding.session &&
		    result->token.id == outstanding.id &&
		    result->token.owner == STOCK_PEER_OWNER_AUDIO &&
		    result->operation == outstanding_operation && !result_count) {
			peer_results[result_count++] = *result;
		}
	}
	k_spin_unlock(&lock, key);
	schedule();
}
ZBUS_LISTENER_DEFINE(audio_listener, input);
ZBUS_CHAN_ADD_OBS(audio_commands, audio_listener, 0);
ZBUS_CHAN_ADD_OBS(stock_peer_states, audio_listener, 0);
ZBUS_CHAN_ADD_OBS(stock_peer_callbacks, audio_listener, 0);
ZBUS_CHAN_ADD_OBS(stock_peer_audio_results, audio_listener, 0);

int audio_request(const struct audio_command *command)
{
	if (!command || !command->session || !command->id) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (publication_inflight || command_busy || command_reserved || command_count ||
	    completion_pending) {
		k_spin_unlock(&lock, key);
		return -EBUSY;
	}
	++command_reserved;
	publication_inflight = true;
	k_spin_unlock(&lock, key);
	int err = zbus_chan_pub(&audio_commands, command, K_NO_WAIT);
	key = k_spin_lock(&lock);
	/* Listener delivery is authoritative even if a later observer failed. */
	bool delivered = command_reserved == 0;
	if (!delivered) {
		--command_reserved;
	}
	publication_inflight = false;
	k_spin_unlock(&lock, key);
	return delivered ? 0 : err;
}
struct audio_state audio_snapshot(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct audio_state copy = state;
	k_spin_unlock(&lock, key);
	return copy;
}
int audio_revoke_raises(uint32_t session)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (!state.session && session && !state.controls_ready) {
		state.session = session;
	}
	if (session != state.session) {
		k_spin_unlock(&lock, key);
		return -ENOENT;
	}
	state.raises_revoked = true;
	state.ready = false;
	state.callbacks_settled = false;
	state_dirty = true;
	k_spin_unlock(&lock, key);
	schedule();
	return 0;
}
/* GPIO drivers here are local register writes; barrier and write share this lock. */
static int pin(unsigned number, bool high)
{
	if (high && state.raises_revoked) {
		return -ECANCELED;
	}
	return audio_hardware_write(number, high);
}
static void fail(int err, const char *reason)
{
	state.error = err;
	failure_reason = reason;
	state.ready = false;
	state.raises_revoked = true;
	state_dirty = true;
	codec_count = 0;
	if (state.controls_ready) {
		(void)pin(16, false);
	}
}
static void respond(const struct stock_peer_callback *callback, int status)
{
	response =
		(struct stock_peer_callback_response){.session = callback->session,
						      .token = callback->token,
						      .status = status,
						      .capabilities_ready = state.controls_ready};
	response_pending = true;
}
static void finish_mode(int status)
{
	respond(&mode_callback, status);
	mode_pending = false;
}
static void finish_codec(void)
{
	if (codec_operation == STOCK_PEER_CODEC_INIT) {
		state.codec_initialized = true;
	}
	codec_running = false;
	codec_second = false;
	state_dirty = true;
}
static void process_callback(const struct stock_peer_callback *callback, int64_t now)
{
	response_enqueues = false;
	if (callback->session != state.session) {
		respond(callback, -ENOENT);
		return;
	}
	switch (callback->kind) {
	case STOCK_PEER_CODEC_CAPABILITIES:
		respond(callback, state.controls_ready ? 0 : -ENOTSUP);
		break;
	case STOCK_PEER_POWER_MODE:
		if (!state.controls_ready || state.raises_revoked ||
		    (callback->mode != 3 && callback->mode != 4) ||
		    (callback->mode == 4 && !state.mode3_ready) ||
		    (callback->mode == 3 && mode_phase >= 2)) {
			respond(callback, -ENOTSUP);
			break;
		}
		if ((callback->mode == 3 && state.mode3_ready) ||
		    (callback->mode == 4 && state.mode4_ready)) {
			respond(callback, 0);
			break;
		}
		mode_callback = *callback;
		mode_pending = true;
		if (callback->mode == 3) {
			int err = pin(16, false);
			if (err) {
				fail(err, "mode 3 GPIO write failed");
				finish_mode(err);
				break;
			}
			mode_phase = 1;
		} else {
			mode_phase = 2;
		}
		mode_due = now + 300;
		break;
	case STOCK_PEER_CODEC_SHUTDOWN:
		if (!state.controls_ready || shutdown_reserved) {
			respond(callback, -ENOTSUP);
			break;
		}
		shutdown_reserved = true;
		respond(callback, 0);
		response_enqueues = true;
		response_operation = callback->kind;
		break;
	case STOCK_PEER_CODEC_INIT:
	case STOCK_PEER_CODEC_ENABLE:
		if (!state.controls_ready || !state.mode4_ready || state.raises_revoked ||
		    codec_count == CODEC_DEPTH) {
			respond(callback, -ENOTSUP);
			break;
		}
		respond(callback, 0);
		response_enqueues = true;
		response_operation = callback->kind;
		break;
	default:
		respond(callback, -ENOTSUP);
		break;
	}
}
static void deadline_handler(struct k_work *item)
{
	ARG_UNUSED(item);
	schedule();
}
static void run(struct k_work *item)
{
	ARG_UNUSED(item);
	int64_t now = k_uptime_get(), next = INT64_MAX;
	struct stock_peer_request request = {0};
	bool send_request = false;
	struct stock_peer_callback_response send_response;
	bool publish_response = false;
	struct audio_result send_completion;
	bool publish_completion = false;
	bool completed_command = false;
	struct audio_state send_state;
	bool publish_state = false;
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (command_count && !command_busy && !completion_pending) {
		struct audio_command command = commands[command_head];
		command_head = (command_head + 1) % COMMAND_DEPTH;
		--command_count;
		int err = 0;
		command_busy = true;
		if (command.operation == AUDIO_PREPARE) {
			if (state.session || peer.session != command.session ||
			    peer.phase != STOCK_PEER_PREPARED || peer.board_type != 1) {
				err = -ENOTSUP;
			} else {
				state = (struct audio_state){.session = command.session,
							     .callbacks_settled = true};
				failure_reason = NULL;
				err = audio_hardware_prepare();
				state.controls_ready = !err;
				if (err) {
					fail(err, "hardware prepare failed");
				}
			}
		} else if (command.operation == AUDIO_QUIESCE) {
			if (!state.session && !state.controls_ready &&
			    peer.session == command.session && peer.phase == STOCK_PEER_HELD) {
				state.session = command.session;
			}
			if (state.session != command.session || peer.session != command.session ||
			    peer.phase != STOCK_PEER_HELD) {
				err = -EPERM;
			} else {
				state.raises_revoked = true;
				/* Preserve an already committed command4 low sequence. */
				if (codec_running && codec_operation == STOCK_PEER_CODEC_SHUTDOWN) {
					commands[command_head] = command;
					++command_count;
					command_busy = false;
					goto commands_done;
				}
				codec_running = false;
				codec_count = 0;
				shutdown_reserved = false;
				err = state.controls_ready ? pin(16, false) : 0;
				state.quiescent = !err;
				if (err) {
					fail(err, "quiesce GPIO write failed");
				}
			}
		} else {
			err = -EINVAL;
		}
		completion =
			(struct audio_result){command.session, command.id, command.operation, err};
		completion_pending = true;
		completed_command = true;
		command_busy = false;
		state_dirty = true;
	}
commands_done:
	if (peer.session == state.session && peer.phase == STOCK_PEER_FAILED) {
		fail(peer.error ? peer.error : -EIO, "stock peer failed");
	}
	if (state.raises_revoked) {
		codec_count = 0;
		if (codec_running && codec_operation != STOCK_PEER_CODEC_SHUTDOWN) {
			codec_running = false;
		}
		if (mode_pending && !response_pending) {
			finish_mode(-ECANCELED);
		}
		if (!mode_pending && !response_pending && !callback_count) {
			state.callbacks_settled = true;
			state_dirty = true;
		}
	}
	for (unsigned i = 0; i < result_count; ++i) {
		struct stock_peer_result *result = &peer_results[i];
		if (!request_outstanding || result->token.session != outstanding.session ||
		    result->token.id != outstanding.id ||
		    result->token.owner != STOCK_PEER_OWNER_AUDIO ||
		    result->operation != outstanding_operation) {
			continue;
		}
		request_outstanding = false;
		if (state.raises_revoked) {
			continue;
		}
		if (result->status) {
			fail(result->status, "stock peer operation failed");
		} else if (result->operation == STOCK_PEER_RAW_ACTIVE) {
			raw_done = true;
		} else if (codec_running && codec_operation == STOCK_PEER_CODEC_INIT) {
			finish_codec();
		}
	}
	result_count = 0;
	if (mode_pending && now >= mode_due && !response_pending) {
		if (mode_phase == 1) {
			state.mode3_ready = true;
			finish_mode(0);
		} else if (mode_phase == 2) {
			int err = pin(16, true);
			if (err) {
				fail(err, "mode 4 GPIO write failed");
				finish_mode(err);
			} else {
				mode_phase = 3;
				mode_due = now + 100;
			}
		} else {
			state.mode4_ready = true;
			finish_mode(0);
		}
		state_dirty = true;
	}
	if (!response_pending && !mode_pending && callback_count) {
		struct stock_peer_callback callback = callbacks[callback_head];
		callback_head = (callback_head + 1) % CALLBACK_DEPTH;
		--callback_count;
		process_callback(&callback, now);
	}
	if (codec_running && codec_second && now >= codec_due) {
		int err = pin(codec_operation == STOCK_PEER_CODEC_SHUTDOWN ? 12 : 16,
			      codec_operation == STOCK_PEER_CODEC_ENABLE);
		if (err) {
			if (err != -ECANCELED) {
				fail(err, "codec second GPIO write failed");
			}
			codec_running = false;
		} else {
			finish_codec();
		}
	}
	if (!codec_running && !state.quiescent) {
		if (shutdown_reserved &&
		    !(response_pending && response_operation == STOCK_PEER_CODEC_SHUTDOWN)) {
			codec_operation = STOCK_PEER_CODEC_SHUTDOWN;
			shutdown_reserved = false;
			codec_running = true;
		} else if (codec_count && !state.raises_revoked && !shutdown_reserved) {
			codec_operation = codec_queue[codec_head];
			codec_head = (codec_head + 1) % CODEC_DEPTH;
			--codec_count;
			codec_running = true;
		}
		if (codec_running) {
			operation_due = now + 5000;
			if (codec_operation != STOCK_PEER_CODEC_INIT) {
				int err =
					pin(codec_operation == STOCK_PEER_CODEC_SHUTDOWN ? 16 : 12,
					    codec_operation == STOCK_PEER_CODEC_ENABLE);
				if (err) {
					if (err != -ECANCELED) {
						fail(err, "codec first GPIO write failed");
					}
					codec_running = false;
				} else {
					codec_second = true;
					codec_due = now + 2;
				}
			}
		}
	}
	if (!state.raises_revoked && state.controls_ready && peer.session == state.session &&
	    peer.readiness >= 2) {
		if (!awake_seen) {
			awake_seen = true;
			readiness_due = now + 6000;
		}
		if (state.mode3_ready && !request_outstanding && !response_pending) {
			if (!state.mode4_ready && !raw_sent) {
				request.operation = STOCK_PEER_RAW_ACTIVE;
				raw_sent = true;
				send_request = true;
			} else if (codec_running && codec_operation == STOCK_PEER_CODEC_INIT) {
				request.operation = STOCK_PEER_DSP_PROFILE;
				request.parameters.profile =
					(peer.run_flags & 9) ? STOCK_PEER_PROFILE_RUN_OVERRIDE
							     : STOCK_PEER_PROFILE_DEFAULT;
				send_request = true;
			}
		}
		if (now >= readiness_due &&
		    !(state.mode4_ready && state.codec_initialized && (!raw_sent || raw_done))) {
			fail(-ETIMEDOUT, "readiness timed out");
		}
	}
	if (codec_running && now >= operation_due) {
		fail(-ETIMEDOUT, "codec operation timed out");
		codec_running = false;
	}
	if (state.raises_revoked) {
		send_request = false;
	}
	if (send_request && next_id == UINT32_MAX) {
		fail(-EOVERFLOW, "stock peer request ID exhausted");
		send_request = false;
	}
	if (send_request) {
		outstanding =
			(struct stock_peer_token){state.session, ++next_id, STOCK_PEER_OWNER_AUDIO};
		request.token = outstanding;
		outstanding_operation = request.operation;
		request_outstanding = true;
	}
	bool ready = state.controls_ready && state.mode3_ready && state.mode4_ready &&
		     state.codec_initialized && (!raw_sent || raw_done) && !state.raises_revoked &&
		     !state.error;
	if (state.ready != ready) {
		state.ready = ready;
		state_dirty = true;
	}
	if (state.raises_revoked && (response_pending || mode_pending || callback_count)) {
		state.callbacks_settled = false;
		state_dirty = true;
	}
	if (response_pending) {
		send_response = response;
		publish_response = true;
	}
	if (completion_pending) {
		send_completion = completion;
		publish_completion = true;
	}
	if (state_dirty) {
		send_state = state;
		state_dirty = false;
		publish_state = true;
	}
	if (mode_pending) {
		next = MIN(next, mode_due);
	}
	if (codec_running) {
		next = MIN(next, operation_due);
		if (codec_second) {
			next = MIN(next, codec_due);
		}
	}
	if (awake_seen && !state.raises_revoked &&
	    !(state.mode4_ready && state.codec_initialized && (!raw_sent || raw_done))) {
		next = MIN(next, readiness_due);
	}
	struct audio_log_snapshot log_snapshot = {
		.state = state,
		.failure_reason = failure_reason,
		.peer_readiness = peer.readiness,
		.codec_operation = codec_operation,
		.peer_operation = outstanding_operation,
		.raw_sent = raw_sent,
		.raw_done = raw_done,
	};
	k_spin_unlock(&lock, key);
	log_state(&log_snapshot);
	if (completed_command && send_completion.status) {
		LOG_ERR("session=%u command=%u operation=%u failed: %d",
			send_completion.session, send_completion.id, send_completion.operation,
			send_completion.status);
	}
	if (send_request) {
		int err = stock_peer_request(&request);
		if (err) {
			key = k_spin_lock(&lock);
			request_outstanding = false;
			if (request.operation == STOCK_PEER_RAW_ACTIVE) {
				raw_sent = false;
			}
			if (err != -EBUSY && err != -ENOBUFS && err != -EAGAIN && err != -ENOSPC) {
				fail(err, "stock peer request rejected");
			}
			k_spin_unlock(&lock, key);
			next = MIN(next, now + 1);
		}
	}
	if (publish_response) {
		int err = stock_peer_callback_respond(&send_response);
		key = k_spin_lock(&lock);
		if (!err || (err == -ENOENT && peer.session == state.session &&
			     peer.phase == STOCK_PEER_HELD)) {
			response_pending = false;
			if (!err && response_enqueues &&
			    response_operation != STOCK_PEER_CODEC_SHUTDOWN &&
			    !state.raises_revoked) {
				codec_queue[(codec_head + codec_count++) % CODEC_DEPTH] =
					response_operation;
			}
			response_enqueues = false;
			if (state.raises_revoked && !mode_pending && !callback_count) {
				state.callbacks_settled = true;
				state_dirty = true;
			}
			k_spin_unlock(&lock, key);
			schedule();
		} else {
			k_spin_unlock(&lock, key);
			next = MIN(next, now + 1);
		}
	}
	if (publish_completion) {
		int err = zbus_chan_pub(&audio_results, &send_completion, K_NO_WAIT);
		if (!err) {
			key = k_spin_lock(&lock);
			completion_pending = false;
			k_spin_unlock(&lock, key);
		} else {
			next = MIN(next, now + 1);
		}
	}
	if (publish_state && zbus_chan_pub(&audio_states, &send_state, K_NO_WAIT)) {
		key = k_spin_lock(&lock);
		state_dirty = true;
		k_spin_unlock(&lock, key);
		next = MIN(next, now + 1);
	}
	key = k_spin_lock(&lock);
	bool more = (callback_count && !mode_pending && !response_pending) ||
		    (command_count && !command_busy && !completion_pending && !codec_running);
	k_spin_unlock(&lock, key);
	if (more) {
		schedule();
	}
	if (next != INT64_MAX) {
		k_work_reschedule(&deadline, K_MSEC(MAX(next - k_uptime_get(), 1)));
	}
}
static int audio_init(void)
{
	k_work_init(&work, run);
	k_work_init_delayable(&deadline, deadline_handler);
	state.raises_revoked = true;
	state.callbacks_settled = true;
	return 0;
}
SYS_INIT(audio_init, APPLICATION, 90);
