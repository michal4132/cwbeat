/* SPDX-License-Identifier: Apache-2.0 */
#include <cwbeat/power.h>
#include <cwbeat/bluetooth.h>
#include <cwbeat/audio.h>
#include <cwbeat/stock_peer.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include "private.h"
LOG_MODULE_REGISTER(cwbeat_power, CONFIG_CWBEAT_POWER_LOG_LEVEL);

static const char *command_name(enum power_command_kind command)
{
	switch (command) {
	case POWER_ON:
		return "on";
	case POWER_OFF:
		return "off";
	case POWER_STOP:
		return "stop";
	case POWER_RESET:
		return "reset";
	case POWER_RECOVERY:
		return "recovery";
	default:
		return "unknown";
	}
}

BUILD_ASSERT(CONFIG_ZBUS_CHANNELS_SYS_INIT_PRIORITY < 90, "power requires initialized channels");
#define CAPACITY 3
static struct k_spinlock lock;
static struct k_work work;
static struct k_work_delayable deadline;
static struct power_state state;
static struct power_command queue[CAPACITY], current;
static unsigned head, count, credits;
static uint32_t next_id, reserved_id, bt_id, bt_session;
static bool terminal_credit, reserved, received, dirty, stopped_this_boot;
static struct power_result results[CAPACITY];
static unsigned result_head, result_count;
static struct bluetooth_result bt_result;
static bool bt_result_pending;
static struct stock_peer_state peer;
static struct audio_state audio;
enum phase {
	IDLE,
	BT_SUBMIT,
	BT_WAIT,
	SAFE_WAIT,
	GRACE,
	DRAIN
};
static enum phase phase;
static int transaction_error;
static int64_t due, drain_deadline, safety_deadline;
ZBUS_CHAN_DEFINE(power_commands, struct power_command, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(power_results, struct power_result, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(power_states, struct power_state, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
static void schedule(void)
{
	k_work_submit(&work);
}
static void input(const struct zbus_channel *channel)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (channel == &power_commands) {
		const struct power_command *command = zbus_chan_const_msg(channel);
		if (reserved && !received && command->id == reserved_id && count < CAPACITY) {
			if (command->command != POWER_ON) {
				head = (head + CAPACITY - 1) % CAPACITY;
				queue[head] = *command;
				++count;
				terminal_credit = true;
			} else {
				queue[(head + count++) % CAPACITY] = *command;
			}
			received = true;
		}
	} else if (channel == &bluetooth_results) {
		const struct bluetooth_result *result = zbus_chan_const_msg(channel);
		if (phase == BT_WAIT && result->id == bt_id &&
		    result->command ==
			    (current.command == POWER_ON ? BLUETOOTH_ENABLE : BLUETOOTH_DISABLE)) {
			bt_result = *result;
			bt_result_pending = true;
		}
	} else if (channel == &stock_peer_states) {
		peer = *(const struct stock_peer_state *)zbus_chan_const_msg(channel);
	} else if (channel == &audio_states) {
		audio = *(const struct audio_state *)zbus_chan_const_msg(channel);
	}
	k_spin_unlock(&lock, key);
	schedule();
}
ZBUS_LISTENER_DEFINE(power_listener, input);
ZBUS_CHAN_ADD_OBS(power_commands, power_listener, 0);
ZBUS_CHAN_ADD_OBS(bluetooth_results, power_listener, 1);
ZBUS_CHAN_ADD_OBS(stock_peer_states, power_listener, 1);
ZBUS_CHAN_ADD_OBS(audio_states, power_listener, 1);
int power_request(enum power_command_kind command, uint32_t *id)
{
	if (command < POWER_ON || command > POWER_RECOVERY) {
		return -EINVAL;
	}
	bool terminal = command != POWER_ON;
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (!terminal && (stopped_this_boot || terminal_credit)) {
		k_spin_unlock(&lock, key);
		return -ENOTSUP;
	}
	if (reserved || credits == CAPACITY || (!terminal && credits >= 2) ||
	    (terminal && terminal_credit)) {
		k_spin_unlock(&lock, key);
		return -EBUSY;
	}
	if (next_id == UINT32_MAX) {
		k_spin_unlock(&lock, key);
		return -EOVERFLOW;
	}
	struct power_command message = {++next_id, command};
	reserved = true;
	received = false;
	reserved_id = message.id;
	++credits;
	k_spin_unlock(&lock, key);
	int err = zbus_chan_pub(&power_commands, &message, K_NO_WAIT);
	key = k_spin_lock(&lock);
	bool delivered = received;
	reserved = false;
	if (!delivered) {
		--credits;
	}
	k_spin_unlock(&lock, key);
	if (delivered && id) {
		*id = message.id;
	}
	return delivered ? 0 : err;
}
struct power_state power_snapshot(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	struct power_state copy = state;
	k_spin_unlock(&lock, key);
	return copy;
}
bool power_autostart_allowed(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool allowed = !state.off_pending && !terminal_credit && !stopped_this_boot;
	k_spin_unlock(&lock, key);
	return allowed;
}
static void complete(int status)
{
	results[(result_head + result_count++) % CAPACITY] =
		(struct power_result){current.id, current.command, status};
	state.transitioning = false;
	state.error = status;
	dirty = true;
	phase = IDLE;
}
/* Cancellation consumes only the completion credits reserved at ingress. */
static void cancel_queued_on(void)
{
	unsigned retained = 0;
	for (unsigned i = 0; i < count; ++i) {
		struct power_command command = queue[(head + i) % CAPACITY];
		if (command.command == POWER_ON) {
			results[(result_head + result_count++) % CAPACITY] =
				(struct power_result){command.id, command.command, -ECANCELED};
		} else {
			queue[(head + retained++) % CAPACITY] = command;
		}
	}
	count = retained;
}
static bool resources_safe(void)
{
	/* A successful DISABLE before the first session closes Bluetooth
	 * admission without creating peer/audio work. The terminal backend
	 * still stops inherited hardware before its reset/handoff. Never use
	 * this exception for a started or partially prepared session. */
	if (!bt_session && !transaction_error && !peer.session &&
	    peer.phase == STOCK_PEER_IDLE && !audio.session && !audio.controls_ready) {
		return true;
	}
	return bt_session && peer.session == bt_session && peer.phase == STOCK_PEER_HELD &&
	       audio.session == bt_session && audio.quiescent && audio.raises_revoked;
}
static void timer(struct k_work *w)
{
	ARG_UNUSED(w);
	schedule();
}
static void run(struct k_work *w)
{
	ARG_UNUSED(w);
	int64_t now = k_uptime_get(), next = INT64_MAX;
	bool submit_bt = false, assert_on = false, terminal = false, publish_result = false,
	     publish_state = false;
	bool started = false, unsafe = false, cleanup_done = false;
	const char *bt_timeout = NULL;
	uint32_t timed_out_bt_id = 0;
	struct stock_peer_state unsafe_peer;
	struct audio_state unsafe_audio;
	enum bluetooth_command_kind bt_command = BLUETOOTH_DISABLE;
	struct power_result result;
	struct power_state snapshot;
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (terminal_credit && !stopped_this_boot) {
		stopped_this_boot = true;
		cancel_queued_on();
	}
	if (terminal_credit && !state.off_pending) {
		state.off_pending = true;
		dirty = true;
	}
	if (phase != IDLE && current.command == POWER_ON && terminal_credit) {
		complete(-ECANCELED);
	}
	if (phase == IDLE && count) {
		current = queue[head];
		head = (head + 1) % CAPACITY;
		--count;
		/* Accepted off intent cancels optional queued autostart before hardware activation.
		 */
		if (current.command == POWER_ON && stopped_this_boot) {
			complete(-ECANCELED);
		} else {
			state.intent = current.command;
			started = true;
			state.transitioning = true;
			state.error = 0;
			dirty = true;
			phase = BT_SUBMIT;
			transaction_error = 0;
			bt_result_pending = false;
			bt_session = 0;
			safety_deadline = now + 35000;
			if (current.command == POWER_ON) {
				assert_on = true;
				state.off_pending = false;
			}
		}
	}
	if (phase == BT_SUBMIT) {
		submit_bt = true;
		bt_command = current.command == POWER_ON ? BLUETOOTH_ENABLE : BLUETOOTH_DISABLE;
	}
	if (phase == BT_WAIT && bt_result_pending) {
		bt_result_pending = false;
		transaction_error = bt_result.status;
		bt_session = bt_result.session;
		if (current.command == POWER_ON) {
			complete(transaction_error);
		} else {
			phase = SAFE_WAIT;
			due = now + 1000;
		}
	}
	if ((phase == BT_WAIT || phase == BT_SUBMIT) && now >= safety_deadline) {
		submit_bt = false;
		bt_timeout = phase == BT_WAIT ? "waiting for Bluetooth result"
					    : "submitting Bluetooth command";
		timed_out_bt_id = phase == BT_WAIT ? bt_id : 0;
		complete(-ETIMEDOUT);
	}
	if (phase == SAFE_WAIT) {
		if (resources_safe()) {
			cleanup_done = true;
			if (current.command == POWER_STOP) {
				complete(transaction_error);
			} else {
				phase = GRACE;
				due = now + 250;
			}
		} else if (now >= due) {
			unsafe = true;
			unsafe_peer = peer;
			unsafe_audio = audio;
			complete(transaction_error ? transaction_error : -EIO);
		} else {
			next = MIN(next, due);
		}
	}
	if (phase == GRACE) {
		if (now >= due) {
			phase = DRAIN;
			drain_deadline = now + 2000;
		} else {
			next = MIN(next, due);
		}
	}
	if (phase == DRAIN) {
		terminal = true;
	}
	if (phase == BT_WAIT || phase == BT_SUBMIT) {
		next = MIN(next, safety_deadline);
	}
	if (result_count) {
		result = results[result_head];
		publish_result = true;
	}
	if (dirty) {
		snapshot = state;
		dirty = false;
		publish_state = true;
	}
	k_spin_unlock(&lock, key);
	if (started) {
		LOG_INF("Request %u: %s", current.id, command_name(current.command));
	}
	if (bt_timeout) {
		LOG_ERR("Request %u: %s timed out %s (Bluetooth request=%u)", current.id,
			command_name(current.command), bt_timeout, timed_out_bt_id);
	}
	if (unsafe) {
		LOG_ERR("Request %u: cleanup not safe (BT session=%u, peer session=%u phase=%u, "
			"audio session=%u quiescent=%u revoked=%u)", current.id, bt_session,
			unsafe_peer.session, unsafe_peer.phase, unsafe_audio.session,
			unsafe_audio.quiescent, unsafe_audio.raises_revoked);
	}
	if (cleanup_done) {
		LOG_INF("Request %u: cleanup complete for %s (status=%d)", current.id,
			command_name(current.command), transaction_error);
	}
	if (assert_on) {
		power_hardware_on();
	}
	if (submit_bt) {
		uint32_t id = 0;
		int err = bluetooth_request(bt_command, &id);
		key = k_spin_lock(&lock);
		if (!err) {
			bt_id = id;
			phase = BT_WAIT;
			next = MIN(next, safety_deadline);
		} else if (err == -EBUSY || err == -ENOBUFS || err == -EAGAIN || err == -ENOSPC) {
			next = MIN(next, now + 1);
		} else {
			complete(err);
		}
		k_spin_unlock(&lock, key);
	}
	if (terminal) {
		bool drained = power_hardware_console_done();
		if (drained || now >= drain_deadline) {
			key = k_spin_lock(&lock);
			bool safe = resources_safe();
			k_spin_unlock(&lock, key);
			if (!safe) {
				LOG_ERR("Request %u: %s refused; resources became unsafe during drain",
					current.id, command_name(current.command));
			}
			int err = safe ? power_hardware_terminal(current.command) : -EIO;
			key = k_spin_lock(&lock);
			complete(err ? err : transaction_error);
			k_spin_unlock(&lock, key);
			schedule();
		} else {
			next = MIN(next, now + 10);
		}
	}
	if (publish_result) {
		int err = zbus_chan_pub(&power_results, &result, K_NO_WAIT);
		if (!err) {
			if (result.status == -ECANCELED) {
				LOG_INF("Request %u: %s canceled by terminal intent", result.id,
					command_name(result.command));
			} else if (result.status) {
				LOG_ERR("Request %u: %s failed (%d)", result.id,
					command_name(result.command), result.status);
			} else {
				LOG_INF("Request %u: %s complete", result.id,
					command_name(result.command));
			}
			key = k_spin_lock(&lock);
			result_head = (result_head + 1) % CAPACITY;
			--result_count;
			--credits;
			if (result.command != POWER_ON) {
				terminal_credit = false;
			}
			k_spin_unlock(&lock, key);
			schedule();
		} else {
			next = MIN(next, now + 1);
		}
	}
	if (publish_state && zbus_chan_pub(&power_states, &snapshot, K_NO_WAIT)) {
		key = k_spin_lock(&lock);
		dirty = true;
		k_spin_unlock(&lock, key);
		next = MIN(next, now + 1);
	}
	key = k_spin_lock(&lock);
	bool more = phase == IDLE && (count || result_count || dirty);
	k_spin_unlock(&lock, key);
	if (more) {
		schedule();
	}
	if (next != INT64_MAX) {
		k_work_reschedule(&deadline, K_MSEC(MAX(next - k_uptime_get(), 1)));
	}
}
static int power_init(void)
{
	k_work_init(&work, run);
	k_work_init_delayable(&deadline, timer);
	state.off_pending = !power_hardware_boot_requested();
	state.intent = state.off_pending ? POWER_OFF : POWER_STOP;
	LOG_INF("Boot power request=%u", !state.off_pending);
	dirty = true;
	schedule();
	return 0;
}
SYS_INIT(power_init, APPLICATION, 90);
