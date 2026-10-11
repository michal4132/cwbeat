/* SPDX-License-Identifier: Apache-2.0 */
#include <cwbeat/bluetooth.h>
#include <cwbeat/audio.h>
#include <cwbeat/stock_peer.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(cwbeat_bluetooth, CONFIG_CWBEAT_BLUETOOTH_LOG_LEVEL);

#if defined(CONFIG_ZBUS_CHANNELS_SYS_INIT_PRIORITY)
BUILD_ASSERT(CONFIG_ZBUS_CHANNELS_SYS_INIT_PRIORITY < 90,
	     "zbus channels must initialize before Bluetooth");
#endif
#define DEPTH       4
#define COMPLETIONS 6
#ifndef BLUETOOTH_SHUTDOWN_MS
#define BLUETOOTH_SHUTDOWN_MS         29550
#define BLUETOOTH_SHUTDOWN_PERIOD_MS  5910
#define BLUETOOTH_SHUTDOWN_ATTEMPT_MS 4910
#endif
struct queued_command {
	struct bluetooth_command command;
	int64_t submitted;
};
K_MSGQ_DEFINE(commands, sizeof(struct queued_command), DEPTH, 4);
K_MSGQ_DEFINE(peer_results, sizeof(struct stock_peer_result), 8, 4);
K_MSGQ_DEFINE(audio_completions, sizeof(struct audio_result), 4, 4);
static struct k_spinlock lock;
struct reservation {
	uint32_t id;
	enum bluetooth_command_kind command;
	bool received, publication_complete, completed;
};
static struct reservation reservations[COMPLETIONS];
static unsigned publishing_normal;
static bool disable_reserved;
static struct bluetooth_state state;
static struct stock_peer_state peer_state;
static struct audio_state audio_state;
static struct bluetooth_result results[COMPLETIONS];
static unsigned result_head, result_count, credits;
static uint32_t sequence, peer_sequence, enable_id, disable_id;
static struct bluetooth_command disable_command;
static bool disable_pending, attempted, state_dirty;
static struct queued_command active;
static bool active_valid;
/* IDs increase within each adapter lane, but separate lanes may finish out of order. */
static uint32_t copied_peer_id[3], copied_audio_id;
static bool completion_overflow;
static uint32_t outstanding;
static enum stock_peer_operation outstanding_operation;
static int64_t deadline, shutdown_started, hold_due;
static int shutdown_slot = -1;
static bool shutdown_sent, hold_requested, audio_quiesce_requested;
static int stop_error;
static struct k_work_delayable work;

static void schedule(void)
{
	k_work_reschedule(&work, K_NO_WAIT);
}
static void retry(void)
{
	k_work_reschedule(&work, K_MSEC(10));
}
static void finish(uint32_t id, enum bluetooth_command_kind command, int status)
{
	if (!id) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	unsigned tail = (result_head + result_count) % COMPLETIONS;
	results[tail] = (struct bluetooth_result){id, state.session, command, status};
	++result_count;
	k_spin_unlock(&lock, key);
}
static void phase(enum bluetooth_phase phase, int error)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	state.phase = phase;
	state.error = error;
	state_dirty = true;
	k_spin_unlock(&lock, key);
}
struct bluetooth_state bluetooth_snapshot(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct bluetooth_state copy = state;
	k_spin_unlock(&lock, key);
	return copy;
}
static void command_listener(const struct zbus_channel *channel)
{
	const struct bluetooth_command *command = zbus_chan_const_msg(channel);
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct reservation *reservation = NULL;
	for (unsigned i = 0; i < COMPLETIONS; ++i) {
		if (reservations[i].id == command->id &&
		    reservations[i].command == command->command) {
			reservation = &reservations[i];
			break;
		}
	}
	if (!reservation || reservation->received) {
		k_spin_unlock(&lock, key);
		return;
	}
	reservation->received = true;
	if (command->command != BLUETOOTH_DISABLE) {
		--publishing_normal;
	}
	if (command->command == BLUETOOTH_DISABLE) {
		disable_command = *command;
		disable_pending = true;
	} else {
		struct queued_command queued = {*command, k_uptime_get()};
		(void)k_msgq_put(&commands, &queued, K_NO_WAIT);
	}
	k_spin_unlock(&lock, key);
	schedule();
}
ZBUS_LISTENER_DEFINE(bt_command_listener, command_listener);
ZBUS_CHAN_DEFINE(bluetooth_commands, struct bluetooth_command, NULL, NULL,
		 ZBUS_OBSERVERS(bt_command_listener), ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(bluetooth_results, struct bluetooth_result, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(bluetooth_states, struct bluetooth_state, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
int bluetooth_request(enum bluetooth_command_kind command, uint32_t *id)
{
	if (!id || command < BLUETOOTH_ENABLE || command > BLUETOOTH_LINK) {
		return -EINVAL;
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool full = credits == COMPLETIONS || sequence == UINT32_MAX ||
		    (command == BLUETOOTH_DISABLE
			     ? disable_reserved
			     : k_msgq_num_used_get(&commands) + publishing_normal == DEPTH ||
				       credits >= COMPLETIONS - 1);
	if (full) {
		k_spin_unlock(&lock, key);
		return -ENOSPC;
	}
	unsigned slot = 0;
	while (slot < COMPLETIONS && reservations[slot].id) {
		++slot;
	}
	if (slot == COMPLETIONS) {
		k_spin_unlock(&lock, key);
		return -ENOSPC;
	}
	struct bluetooth_command request = {++sequence, command};
	reservations[slot] = (struct reservation){request.id, command, false};
	++credits;
	if (command == BLUETOOTH_DISABLE) {
		disable_reserved = true;
	} else {
		++publishing_normal;
	}
	k_spin_unlock(&lock, key);
	int status = zbus_chan_pub(&bluetooth_commands, &request, K_NO_WAIT);
	key = k_spin_lock(&lock);
	if (reservations[slot].received) {
		status = 0;
	}
	reservations[slot].publication_complete = true;
	if (status) {
		reservations[slot].id = 0;
		--credits;
		if (command == BLUETOOTH_DISABLE) {
			disable_reserved = false;
		} else {
			--publishing_normal;
		}
	} else {
		*id = request.id;
		if (reservations[slot].completed) {
			reservations[slot].id = 0;
		}
	}
	k_spin_unlock(&lock, key);
	return status;
}
static void peer_result_listener(const struct zbus_channel *channel)
{
	const struct stock_peer_result *result = zbus_chan_const_msg(channel);
	if (result->token.owner != STOCK_PEER_OWNER_BLUETOOTH || !result->token.id ||
	    (int)result->operation < 0 || result->operation > STOCK_PEER_LINK) {
		return;
	}
	unsigned lane = result->operation == STOCK_PEER_HOLD       ? 2
			: result->operation <= STOCK_PEER_SHUTDOWN ? 1
								   : 0;
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (result->token.session != state.session || result->token.id <= copied_peer_id[lane]) {
		k_spin_unlock(&lock, key);
		return;
	}
	if (k_msgq_put(&peer_results, result, K_NO_WAIT)) {
		completion_overflow = true;
	} else {
		copied_peer_id[lane] = result->token.id;
	}
	k_spin_unlock(&lock, key);
	schedule();
}
static void audio_result_listener(const struct zbus_channel *channel)
{
	const struct audio_result *result = zbus_chan_const_msg(channel);
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (result->session != state.session || !result->id || result->id <= copied_audio_id) {
		k_spin_unlock(&lock, key);
		return;
	}
	if (k_msgq_put(&audio_completions, result, K_NO_WAIT)) {
		completion_overflow = true;
	} else {
		copied_audio_id = result->id;
	}
	k_spin_unlock(&lock, key);
	schedule();
}
static void peer_state_listener(const struct zbus_channel *channel)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const struct stock_peer_state *incoming = zbus_chan_const_msg(channel);
	if (state.session && incoming->session != state.session) {
		k_spin_unlock(&lock, key);
		return;
	}
	peer_state = *incoming;
	k_spin_unlock(&lock, key);
	schedule();
}
static void audio_state_listener(const struct zbus_channel *channel)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	const struct audio_state *incoming = zbus_chan_const_msg(channel);
	if (state.session && incoming->session != state.session) {
		k_spin_unlock(&lock, key);
		return;
	}
	audio_state = *incoming;
	k_spin_unlock(&lock, key);
	schedule();
}
ZBUS_LISTENER_DEFINE(bt_peer_result_listener, peer_result_listener);
ZBUS_LISTENER_DEFINE(bt_audio_result_listener, audio_result_listener);
ZBUS_LISTENER_DEFINE(bt_peer_state_listener, peer_state_listener);
ZBUS_LISTENER_DEFINE(bt_audio_state_listener, audio_state_listener);
ZBUS_CHAN_ADD_OBS(stock_peer_bt_results, bt_peer_result_listener, 0);
ZBUS_CHAN_ADD_OBS(audio_results, bt_audio_result_listener, 0);
ZBUS_CHAN_ADD_OBS(stock_peer_states, bt_peer_state_listener, 0);
ZBUS_CHAN_ADD_OBS(audio_states, bt_audio_state_listener, 0);

static int send(enum stock_peer_operation operation)
{
	struct stock_peer_request request = {
		.token = {state.session, ++peer_sequence, STOCK_PEER_OWNER_BLUETOOTH},
		.operation = operation};
	int status = stock_peer_request(&request);
	if (!status) {
		outstanding = request.token.id;
		outstanding_operation = operation;
		LOG_DBG("Peer request: session=%u id=%u operation=%u", request.token.session,
			request.token.id, operation);
	}
	return status;
}
static void cancel_commands(void)
{
	if (active_valid) {
		finish(active.command.id, active.command.command, -ECANCELED);
		active_valid = false;
	}
	struct queued_command queued;
	for (unsigned i = 0; i < DEPTH && !k_msgq_get(&commands, &queued, K_NO_WAIT); ++i) {
		finish(queued.command.id, queued.command.command, -ECANCELED);
	}
	outstanding = 0;
}
static void begin_stop(uint32_t id, int error)
{
	disable_id = id;
	stop_error = error;
	k_spinlock_key_t key = k_spin_lock(&lock);
	state.admission_closed = true;
	k_spin_unlock(&lock, key);
	int peer_error = state.session ? stock_peer_close_admission(state.session) : 0;
	int audio_error = state.session ? audio_revoke_raises(state.session) : 0;
	if (peer_error || audio_error) {
		stop_error = peer_error ? peer_error : audio_error;
		LOG_ERR("Admission close failed: session=%u peer=%d audio=%d", state.session,
			peer_error, audio_error);
	}
	LOG_INF("Stopping: session=%u disable=%u phase=%u error=%d", state.session, id,
		state.phase, stop_error);
	cancel_commands();
	if (enable_id) {
		finish(enable_id, BLUETOOTH_ENABLE, error ? error : -ECANCELED);
		enable_id = 0;
	}
	shutdown_started = k_uptime_get();
	shutdown_slot = -1;
	hold_due = 0;
	shutdown_sent = hold_requested = audio_quiesce_requested = false;
	deadline = shutdown_started + BLUETOOTH_SHUTDOWN_MS;
	phase(BLUETOOTH_STOPPING, stop_error);
}
static void fault(int error)
{
	if (state.phase == BLUETOOTH_STOPPING) {
		stop_error = error;
		outstanding = 0;
		return;
	}
	begin_stop(0, error);
}
static int media_operation(enum bluetooth_command_kind command, const struct stock_peer_state *peer,
			   enum stock_peer_operation *operation)
{
	switch (command) {
	case BLUETOOTH_VOLUME_UP:
		*operation = STOCK_PEER_VOLUME_UP;
		return 0;
	case BLUETOOTH_VOLUME_DOWN:
		*operation = STOCK_PEER_VOLUME_DOWN;
		return 0;
	case BLUETOOTH_DISCOVER:
		*operation = STOCK_PEER_DISCOVER;
		return 0;
	case BLUETOOTH_PAIR:
		*operation = STOCK_PEER_PAIR;
		return 0;
	case BLUETOOTH_LINK:
		*operation = STOCK_PEER_LINK;
		return 0;
	default:
		break;
	}
	if (!peer->connection) {
		return -ENOTCONN;
	}
	if (command == BLUETOOTH_PLAY_PAUSE) {
		if (peer->auxiliary == 2 || (peer->playing == 1 && peer->connection != 2)) {
			return -ENOTSUP;
		}
		*operation = peer->playing == 1 ? STOCK_PEER_PAUSE : STOCK_PEER_PLAY;
	} else {
		if (peer->auxiliary >= 2) {
			return -ENOTSUP;
		}
		*operation = command == BLUETOOTH_NEXT ? STOCK_PEER_NEXT : STOCK_PEER_PREVIOUS;
	}
	return 0;
}
static void deliver(void)
{
	for (unsigned i = 0; i < COMPLETIONS; ++i) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		bool available = result_count != 0;
		struct bluetooth_result result = results[result_head];
		k_spin_unlock(&lock, key);
		if (!available) {
			break;
		}
		if (zbus_chan_pub(&bluetooth_results, &result, K_NO_WAIT)) {
			retry();
			break;
		}
		key = k_spin_lock(&lock);
		result_head = (result_head + 1) % COMPLETIONS;
		--result_count;
		--credits;
		for (unsigned slot = 0; slot < COMPLETIONS; ++slot) {
			if (reservations[slot].id == result.id) {
				reservations[slot].completed = true;
				if (reservations[slot].publication_complete) {
					reservations[slot].id = 0;
				}
				break;
			}
		}
		if (result.command == BLUETOOTH_DISABLE) {
			disable_reserved = false;
		}
		k_spin_unlock(&lock, key);
	}
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool dirty = state_dirty;
	struct bluetooth_state copy = state;
	state_dirty = false;
	k_spin_unlock(&lock, key);
	if (dirty && zbus_chan_pub(&bluetooth_states, &copy, K_NO_WAIT)) {
		key = k_spin_lock(&lock);
		state_dirty = true;
		k_spin_unlock(&lock, key);
		retry();
	}
}

enum enable_stage {
	START_PREPARE,
	START_AUDIO,
	START_RELEASE,
	START_PREREQUISITES,
	START_FEEDBACK,
	START_WAKE,
	START_AWAKE,
	START_DONE
};
static enum enable_stage stage;
static bool audio_prepare_requested;
static inline const char *stage_name(enum enable_stage value)
{
	switch (value) {
	case START_PREPARE:
		return "prepare";
	case START_AUDIO:
		return "audio-prepare";
	case START_RELEASE:
		return "peer-release";
	case START_PREREQUISITES:
		return "prerequisites";
	case START_FEEDBACK:
		return "feedback-disable";
	case START_WAKE:
		return "wake";
	case START_AWAKE:
		return "awake";
	case START_DONE:
		return "done";
	default:
		return "unknown";
	}
}
static void handle_result(const struct stock_peer_result *result)
{
	if (result->token.session != state.session || result->token.id != outstanding ||
	    result->operation != outstanding_operation) {
		return;
	}
	outstanding = 0;
	if (result->status) {
		LOG_ERR("Peer result failed: session=%u id=%u operation=%u phase=%u error=%d",
			result->token.session, result->token.id, result->operation, state.phase,
			result->status);
		if (state.phase == BLUETOOTH_STOPPING && result->operation == STOCK_PEER_HOLD) {
			phase(BLUETOOTH_FAILED, result->status);
			finish(disable_id, BLUETOOTH_DISABLE, result->status);
			disable_id = 0;
		} else if (active_valid) {
			finish(active.command.id, active.command.command, result->status);
			active_valid = false;
		} else {
			fault(result->status);
		}
		return;
	}
	if (active_valid) {
		finish(active.command.id, active.command.command, 0);
		active_valid = false;
		return;
	}
	switch (result->operation) {
	case STOCK_PEER_PREPARE:
		stage = START_AUDIO;
		break;
	case STOCK_PEER_RELEASE:
		stage = START_PREREQUISITES;
		phase(BLUETOOTH_ENABLING, 0);
		LOG_INF("Peer released: session=%u; waiting for readiness and audio mode 3",
			state.session);
		break;
	case STOCK_PEER_FEEDBACK_DISABLE:
		stage = START_WAKE;
		break;
	case STOCK_PEER_WAKE:
		stage = START_AWAKE;
		break;
	case STOCK_PEER_SHUTDOWN:
		shutdown_sent = true;
		break;
	case STOCK_PEER_HOLD:
		hold_requested = true;
		break;
	default:
		break;
	}
}
static void advance_start(const struct stock_peer_state *peer, const struct audio_state *audio)
{
	if (k_uptime_get() >= deadline) {
		LOG_ERR("Enable timeout: session=%u id=%u stage=%s outstanding=%u operation=%u",
			state.session, enable_id, stage_name(stage), outstanding,
			outstanding_operation);
		LOG_ERR("Prerequisites: peer session=%u phase=%u readiness=%u error=%d; "
			"audio session=%u controls=%u mode3=%u revoked=%u error=%d",
			peer->session, peer->phase, peer->readiness, peer->error, audio->session,
			audio->controls_ready, audio->mode3_ready, audio->raises_revoked, audio->error);
		fault(-ETIMEDOUT);
		return;
	}
	if (outstanding) {
		k_work_schedule(&work, K_TIMEOUT_ABS_MS(deadline));
		return;
	}
	int status = 0;
	switch (stage) {
	case START_PREPARE:
		status = send(STOCK_PEER_PREPARE);
		break;
	case START_AUDIO:
		if (peer->session != state.session || peer->phase != STOCK_PEER_PREPARED) {
			break;
		}
		if (!audio_prepare_requested) {
			struct audio_command request = {state.session, ++peer_sequence,
							AUDIO_PREPARE};
			status = audio_request(&request);
			if (!status) {
				audio_prepare_requested = true;
			}
		}
		if (audio->session == state.session && audio->controls_ready) {
			stage = START_RELEASE;
			schedule();
		}
		break;
	case START_RELEASE:
		status = send(STOCK_PEER_RELEASE);
		break;
	case START_PREREQUISITES:
		if (peer->session == state.session && peer->readiness >= 1 &&
		    audio->session == state.session && audio->mode3_ready) {
			stage = IS_ENABLED(CONFIG_CWBEAT_BLUETOOTH_DISABLE_FEEDBACK)
					? START_FEEDBACK
					: START_WAKE;
			deadline = k_uptime_get() + 5000;
			schedule();
		}
		break;
	case START_FEEDBACK:
		status = send(STOCK_PEER_FEEDBACK_DISABLE);
		break;
	case START_WAKE:
		if (peer->readiness >= 2) {
			stage = START_AWAKE;
			schedule();
		} else {
			status = send(STOCK_PEER_WAKE);
		}
		break;
	case START_AWAKE:
		if (peer->session == state.session && peer->readiness >= 2 &&
		    audio->session == state.session && audio->mode3_ready) {
			stage = START_DONE;
			phase(BLUETOOTH_AWAKE, 0);
			LOG_INF("Awake: session=%u enable=%u readiness=%u", state.session, enable_id,
				peer->readiness);
			finish(enable_id, BLUETOOTH_ENABLE, 0);
			enable_id = 0;
			schedule();
		}
		break;
	default:
		break;
	}
	if (status == -ENOSPC || status == -EAGAIN || status == -EBUSY) {
		retry();
	} else if (status) {
		LOG_ERR("Enable request failed: session=%u stage=%s error=%d", state.session,
			stage_name(stage), status);
		fault(status);
	} else if (state.phase != BLUETOOTH_AWAKE) {
		k_work_schedule(&work, K_TIMEOUT_ABS_MS(deadline));
	}
}
static void advance_stop(const struct stock_peer_state *peer, const struct audio_state *audio)
{
	int64_t now = k_uptime_get();
	if (!state.session) {
		LOG_INF("Stopped before peer session: disable=%u error=%d", disable_id, stop_error);
		phase(BLUETOOTH_STOPPED, stop_error);
		finish(disable_id, BLUETOOTH_DISABLE, stop_error);
		disable_id = 0;
		return;
	}
	bool timed_out = now >= shutdown_started + BLUETOOTH_SHUTDOWN_MS;
	if (timed_out && !stop_error) {
		LOG_ERR("Native shutdown timeout: session=%u readiness=%u peer_phase=%u "
			"audio_revoked=%u callbacks_settled=%u",
			state.session, peer->readiness, peer->phase, audio->raises_revoked,
			audio->callbacks_settled);
		stop_error = -ETIMEDOUT;
	}
	bool shutdown_done = peer->session == state.session && peer->readiness == 1;
	bool held = peer->session == state.session && peer->phase == STOCK_PEER_HELD;
	if (!held && !hold_requested &&
	    (timed_out || shutdown_done || stop_error || peer->phase == STOCK_PEER_PREPARED)) {
		if (!outstanding || outstanding_operation == STOCK_PEER_SHUTDOWN) {
			outstanding = 0;
			if (!hold_due) {
				hold_due = now + 1000;
			}
			if (now >= hold_due) {
				LOG_ERR("Hold admission timeout: session=%u disable=%u", state.session,
					disable_id);
				phase(BLUETOOTH_FAILED, -ETIMEDOUT);
				finish(disable_id, BLUETOOTH_DISABLE, -ETIMEDOUT);
				disable_id = 0;
				return;
			}
			int status = send(STOCK_PEER_HOLD);
			if (!status) {
				hold_requested = true;
				deadline = hold_due;
			}
			if (status) {
				if (status == -ENOSPC || status == -EAGAIN || status == -EBUSY) {
					retry();
				} else {
					LOG_ERR("Hold request failed: session=%u error=%d", state.session,
						status);
					phase(BLUETOOTH_FAILED, status);
					finish(disable_id, BLUETOOTH_DISABLE, status);
					disable_id = 0;
				}
			}
		}
	} else if (!held && hold_requested) {
		if (now >= deadline) {
			LOG_ERR("Hold completion timeout: session=%u disable=%u peer_phase=%u",
				state.session, disable_id, peer->phase);
			phase(BLUETOOTH_FAILED, -ETIMEDOUT);
			finish(disable_id, BLUETOOTH_DISABLE, -ETIMEDOUT);
			disable_id = 0;
		} else {
			k_work_schedule(&work, K_TIMEOUT_ABS_MS(deadline));
		}
	} else if (held && !audio_quiesce_requested) {
		struct audio_command request = {state.session, ++peer_sequence, AUDIO_QUIESCE};
		int status = audio_request(&request);
		if (!status) {
			audio_quiesce_requested = true;
			deadline = now + 1000;
		} else if (now >= deadline ||
			   (status != -ENOSPC && status != -EAGAIN && status != -EBUSY)) {
			int error = now >= deadline ? -ETIMEDOUT : status;
			LOG_ERR("Audio quiesce admission failed: session=%u request_error=%d error=%d",
				state.session, status, error);
			phase(BLUETOOTH_FAILED, error);
			finish(disable_id, BLUETOOTH_DISABLE, error);
			disable_id = 0;
		} else {
			retry();
		}
	} else if (held && audio_quiesce_requested) {
		if (audio->session == state.session && audio->quiescent) {
			LOG_INF("Stopped: session=%u disable=%u held=1 quiescent=1 error=%d",
				state.session, disable_id, stop_error);
			phase(BLUETOOTH_STOPPED, stop_error);
			finish(disable_id, BLUETOOTH_DISABLE, stop_error);
			disable_id = 0;
		} else if (now >= deadline) {
			LOG_ERR("Audio quiesce timeout: session=%u audio_session=%u quiescent=%u "
				"revoked=%u error=%d",
				state.session, audio->session, audio->quiescent, audio->raises_revoked,
				audio->error);
			phase(BLUETOOTH_FAILED, -ETIMEDOUT);
			finish(disable_id, BLUETOOTH_DISABLE, -ETIMEDOUT);
			disable_id = 0;
		} else {
			k_work_schedule(&work, K_TIMEOUT_ABS_MS(deadline));
		}
	} else if (!outstanding && audio->session == state.session && audio->raises_revoked &&
		   audio->callbacks_settled) {
		int64_t elapsed = now - shutdown_started;
		int slot = elapsed / BLUETOOTH_SHUTDOWN_PERIOD_MS;
		int offset = elapsed % BLUETOOTH_SHUTDOWN_PERIOD_MS;
		if (slot != shutdown_slot && offset < BLUETOOTH_SHUTDOWN_ATTEMPT_MS) {
			int status = send(STOCK_PEER_SHUTDOWN);
			if (!status) {
				shutdown_slot = slot;
			} else if (status != -EAGAIN && status != -ENOSPC && status != -EBUSY) {
				LOG_ERR("Shutdown request failed: session=%u error=%d", state.session,
					status);
				stop_error = status;
				schedule();
			} else {
				retry();
				return;
			}
		}
		k_work_schedule(&work, K_TIMEOUT_ABS_MS(shutdown_started +
							(slot + 1) * BLUETOOTH_SHUTDOWN_PERIOD_MS));
	}
	if (state.phase == BLUETOOTH_STOPPING && !hold_requested &&
	    !k_work_delayable_is_pending(&work)) {
		k_work_schedule(&work, K_TIMEOUT_ABS_MS(shutdown_started + BLUETOOTH_SHUTDOWN_MS));
	}
}
static void run(struct k_work *item)
{
	ARG_UNUSED(item);
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool disable = disable_pending;
	bool overflow = completion_overflow;
	completion_overflow = false;
	struct bluetooth_command stop = disable_command;
	disable_pending = false;
	struct stock_peer_state peer = peer_state;
	struct audio_state audio = audio_state;
	k_spin_unlock(&lock, key);
	if (disable) {
		if (state.phase == BLUETOOTH_STOPPED || state.phase == BLUETOOTH_DISABLED) {
			/* An off intent consumes this boot's only session opportunity. */
			attempted = true;
			phase(BLUETOOTH_STOPPED, 0);
			LOG_INF("Already stopped: session=%u disable=%u", state.session, stop.id);
			finish(stop.id, BLUETOOTH_DISABLE, 0);
		} else if (state.phase == BLUETOOTH_STOPPING) {
			/* An internal fault can already be cleaning up without a DISABLE
			 * owner. Admission reserves only one external DISABLE at a time,
			 * so let this request await the existing cleanup and its error. */
			disable_id = stop.id;
			LOG_INF("Disable joins cleanup: session=%u disable=%u error=%d", state.session,
				stop.id, stop_error);
		} else {
			begin_stop(stop.id, 0);
		}
	}
	if (overflow) {
		LOG_ERR("Completion queue overflow: session=%u phase=%u", state.session, state.phase);
		fault(-ENOBUFS);
	}
	struct stock_peer_result result;
	for (unsigned i = 0; i < 8 && !k_msgq_get(&peer_results, &result, K_NO_WAIT); ++i) {
		handle_result(&result);
	}
	struct audio_result audio_result;
	for (unsigned i = 0; i < 4 && !k_msgq_get(&audio_completions, &audio_result, K_NO_WAIT);
	     ++i) {
		if (audio_result.session == state.session && audio_result.status &&
		    state.phase != BLUETOOTH_STOPPED) {
			LOG_ERR("Audio result failed: session=%u id=%u operation=%u error=%d",
				audio_result.session, audio_result.id, audio_result.operation,
				audio_result.status);
			fault(audio_result.status);
		}
	}
	if (peer.session == state.session) {
		key = k_spin_lock(&lock);
		if (state.readiness != peer.readiness || state.connection != peer.connection ||
		    state.playing != peer.playing || state.auxiliary != peer.auxiliary) {
			state_dirty = true;
		}
		state.readiness = peer.readiness;
		state.connection = peer.connection;
		state.playing = peer.playing;
		state.auxiliary = peer.auxiliary;
		k_spin_unlock(&lock, key);
		if (peer.phase == STOCK_PEER_FAILED && state.phase != BLUETOOTH_STOPPING &&
		    state.phase != BLUETOOTH_STOPPED) {
			LOG_ERR("Peer fault: session=%u readiness=%u error=%d", peer.session,
				peer.readiness, peer.error);
			fault(peer.error ? peer.error : -EIO);
		}
	}
	if (audio.session == state.session && audio.error && state.phase != BLUETOOTH_STOPPING &&
	    state.phase != BLUETOOTH_STOPPED) {
		LOG_ERR("Audio fault: session=%u controls=%u mode3=%u mode4=%u codec=%u error=%d",
			audio.session, audio.controls_ready, audio.mode3_ready, audio.mode4_ready,
			audio.codec_initialized, audio.error);
		fault(audio.error);
	}
	if (state.phase == BLUETOOTH_STOPPING) {
		advance_stop(&peer, &audio);
	} else if (state.phase == BLUETOOTH_PREPARING || state.phase == BLUETOOTH_ENABLING) {
		advance_start(&peer, &audio);
	} else if (!outstanding && !active_valid) {
		struct queued_command queued;
		for (unsigned i = 0; i < DEPTH && !k_msgq_get(&commands, &queued, K_NO_WAIT); ++i) {
			enum bluetooth_command_kind command = queued.command.command;
			if (command == BLUETOOTH_ENABLE) {
				if (attempted) {
					LOG_WRN("Enable rejected: id=%u session already consumed this boot",
						queued.command.id);
					finish(queued.command.id, command, -EALREADY);
					continue;
				}
				attempted = true;
				enable_id = queued.command.id;
				key = k_spin_lock(&lock);
				state.session = 1;
				state.admission_closed = false;
				k_spin_unlock(&lock, key);
				stage = START_PREPARE;
				audio_prepare_requested = false;
				deadline = k_uptime_get() + 6000;
				phase(BLUETOOTH_PREPARING, 0);
				LOG_INF("Enabling: session=%u id=%u", state.session, enable_id);
				advance_start(&peer, &audio);
				break;
			}
			if (state.phase != BLUETOOTH_AWAKE || state.admission_closed) {
				finish(queued.command.id, command, -EHOSTDOWN);
				continue;
			}
			if (k_uptime_get() - queued.submitted >= 1000) {
				finish(queued.command.id, command, -ETIMEDOUT);
				continue;
			}
			enum stock_peer_operation operation;
			int status = media_operation(command, &peer, &operation);
			if (!status) {
				status = send(operation);
			}
			if (status == -ENOSPC || status == -EAGAIN || status == -EBUSY) {
				/* Retain the original timestamp and FIFO position while transport
				 * is full. */
				active = queued;
				active_valid = true;
				retry();
				break;
			}
			if (status) {
				finish(queued.command.id, command, status);
				continue;
			}
			active = queued;
			active_valid = true;
			break;
		}
	}
	if (active_valid && !outstanding && state.phase == BLUETOOTH_AWAKE) {
		enum stock_peer_operation operation;
		int status = k_uptime_get() - active.submitted >= 1000
				     ? -ETIMEDOUT
				     : media_operation(active.command.command, &peer, &operation);
		if (!status) {
			status = send(operation);
		}
		if (status == -ENOSPC || status == -EAGAIN || status == -EBUSY) {
			retry();
		} else if (status) {
			finish(active.command.id, active.command.command, status);
			active_valid = false;
			schedule();
		}
	}
	if (!outstanding && !active_valid && k_msgq_num_used_get(&commands) &&
	    state.phase == BLUETOOTH_AWAKE) {
		schedule();
	}
	deliver();
}
static int initialize(void)
{
	k_work_init_delayable(&work, run);
	state.phase = BLUETOOTH_DISABLED;
	state.admission_closed = true;
	return 0;
}
SYS_INIT(initialize, APPLICATION, 90);

#ifdef BLUETOOTH_TEST
/* Fixture-only storage observation: the workqueue is blocked by the test. */
unsigned bluetooth_test_peer_completion_count(void)
{
	return k_msgq_num_used_get(&peer_results);
}
unsigned bluetooth_test_audio_completion_count(void)
{
	return k_msgq_num_used_get(&audio_completions);
}
#endif
