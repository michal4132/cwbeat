/* SPDX-License-Identifier: Apache-2.0 */
#include "private.h"
#include <cwbeat/bluetooth.h>
#include <cwbeat/power.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/led.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <errno.h>

#define HISTORY_DEPTH 32
#define BOOT_GRACE_MS 400
struct button_event {
	uint32_t when;
	enum cwbeat_button button;
	bool down;
};
K_MSGQ_DEFINE(history, sizeof(struct button_event), HISTORY_DEPTH, 4);
static struct k_spinlock lock;
static uint32_t held;
static bool lost, initialized;
static int64_t boot_time;
static struct bluetooth_state bluetooth;
static struct power_state power;
static struct gestures gestures;
static void process(struct k_work *work);
K_WORK_DEFINE(input_work, process);
static void deadline_expired(struct k_work *work)
{
	ARG_UNUSED(work);
	k_work_submit(&input_work);
}
K_WORK_DELAYABLE_DEFINE(deadline, deadline_expired);
BUILD_ASSERT(CONFIG_ZBUS_CHANNELS_SYS_INIT_PRIORITY < 90, "zbus must initialize before UI");

__weak int ui_hardware_led(unsigned channel, bool on)
{
#if DT_HAS_CHOSEN(cwbeat_leds)
	const struct device *device = DEVICE_DT_GET(DT_CHOSEN(cwbeat_leds));
	if (!device_is_ready(device)) {
		return -ENODEV;
	}
	return on ? led_on(device, channel) : led_off(device, channel);
#else
	ARG_UNUSED(channel);
	ARG_UNUSED(on);
	return 0;
#endif
}
void ui_input_event(enum cwbeat_button button, bool down, uint32_t when)
{
	if (button < BUTTON_PLAY || button >= BUTTON_COUNT) {
		return;
	}
	struct button_event event = {when, button, down};
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (down) {
		held |= BIT(button);
	} else {
		held &= ~BIT(button);
	}
	if (!lost && k_msgq_put(&history, &event, K_NO_WAIT)) {
		lost = true;
	}
	bool ready = initialized;
	k_spin_unlock(&lock, key);
	if (ready) {
		k_work_submit(&input_work);
	}
}
#if DT_HAS_CHOSEN(cwbeat_buttons)
static void input(struct input_event *event, void *unused)
{
	ARG_UNUSED(unused);
	if (event->type != INPUT_EV_KEY) {
		return;
	}
	enum cwbeat_button button;
	switch (event->code) {
	case INPUT_KEY_PLAYPAUSE:
		button = BUTTON_PLAY;
		break;
	case INPUT_KEY_BLUETOOTH:
		button = BUTTON_BT;
		break;
	case INPUT_KEY_VOLUMEUP:
		button = BUTTON_UP;
		break;
	case INPUT_KEY_VOLUMEDOWN:
		button = BUTTON_DOWN;
		break;
	case INPUT_KEY_POWER:
		button = BUTTON_POWER;
		break;
	case INPUT_KEY_CONNECT:
		button = BUTTON_LINK;
		break;
	default:
		return;
	}
	ui_input_event(button, event->value != 0, k_uptime_get_32());
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_CHOSEN(cwbeat_buttons)), input, NULL);
#endif
static void observe_bluetooth(const struct zbus_channel *channel)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bluetooth = *(const struct bluetooth_state *)zbus_chan_const_msg(channel);
	bool ready = initialized;
	k_spin_unlock(&lock, key);
	if (ready) {
		k_work_submit(&input_work);
	}
}
static void observe_power(const struct zbus_channel *channel)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	power = *(const struct power_state *)zbus_chan_const_msg(channel);
	bool ready = initialized;
	k_spin_unlock(&lock, key);
	if (ready) {
		k_work_submit(&input_work);
	}
}
ZBUS_LISTENER_DEFINE(ui_bluetooth_listener, observe_bluetooth);
ZBUS_LISTENER_DEFINE(ui_power_listener, observe_power);
ZBUS_CHAN_ADD_OBS(bluetooth_states, ui_bluetooth_listener, 0);
ZBUS_CHAN_ADD_OBS(power_states, ui_power_listener, 0);
static void emit(enum gesture_action action)
{
	uint32_t id;
	if (action == GESTURE_ON) {
		(void)power_request(POWER_ON, &id);
		return;
	}
	if (action == GESTURE_OFF) {
		(void)power_request(POWER_OFF, &id);
		return;
	}
	static const enum bluetooth_command_kind commands[] = {
		[GESTURE_PLAY_PAUSE] = BLUETOOTH_PLAY_PAUSE,
		[GESTURE_NEXT] = BLUETOOTH_NEXT,
		[GESTURE_PREVIOUS] = BLUETOOTH_PREVIOUS,
		[GESTURE_UP] = BLUETOOTH_VOLUME_UP,
		[GESTURE_DOWN] = BLUETOOTH_VOLUME_DOWN,
		[GESTURE_DISCOVER] = BLUETOOTH_DISCOVER,
		[GESTURE_PAIR] = BLUETOOTH_PAIR,
		[GESTURE_LINK] = BLUETOOTH_LINK};
	if (action < ARRAY_SIZE(commands)) {
		(void)bluetooth_request(commands[action], &id);
	}
}
static void process(struct k_work *work)
{
	ARG_UNUSED(work);
	k_spinlock_key_t key = k_spin_lock(&lock);
	if (!initialized) {
		k_spin_unlock(&lock, key);
		return;
	}
	struct bluetooth_state bt = bluetooth;
	struct power_state board = power;
	k_spin_unlock(&lock, key);
	uint32_t now = k_uptime_get_32();
	enum gestures_mode mode = GESTURES_DISABLED;
	bool qualified = k_uptime_get() - boot_time >= BOOT_GRACE_MS;
	if (qualified) {
		if (bt.phase == BLUETOOTH_AWAKE && !bt.admission_closed) {
			mode = GESTURES_RUNNING;
		} else if (board.intent == POWER_OFF && !board.transitioning) {
			mode = GESTURES_OFF;
		} else if (board.error || bt.phase == BLUETOOTH_STOPPED ||
			   bt.phase == BLUETOOTH_FAILED) {
			mode = GESTURES_POWER_ONLY;
		}
	}
	if (mode != gestures.mode) {
		key = k_spin_lock(&lock);
		k_msgq_purge(&history);
		uint32_t levels = held;
		lost = false;
		k_spin_unlock(&lock, key);
		gestures_state(&gestures, mode);
		gestures_resync(&gestures, levels);
	} else {
		gestures_state(&gestures, mode);
	}
	for (unsigned i = 0; i < HISTORY_DEPTH; ++i) {
		struct button_event event;
		key = k_spin_lock(&lock);
		bool resync = lost;
		uint32_t levels = held;
		if (resync) {
			k_msgq_purge(&history);
			lost = false;
		}
		int status = resync ? 0 : k_msgq_get(&history, &event, K_NO_WAIT);
		k_spin_unlock(&lock, key);
		if (resync) {
			gestures_resync(&gestures, levels);
		} else if (status) {
			break;
		} else {
			gestures_event(&gestures, event.button, event.down, event.when);
		}
	}
	gestures_poll(&gestures, now);
	uint32_t delay = gestures_timeout(&gestures, now);
	if (!qualified) {
		delay = MIN(delay, (uint32_t)MAX(0, BOOT_GRACE_MS - (k_uptime_get() - boot_time)));
	}
	if (delay == UINT32_MAX) {
		(void)k_work_cancel_delayable(&deadline);
	} else {
		k_work_reschedule(&deadline, K_MSEC(delay));
	}
	key = k_spin_lock(&lock);
	bool more = lost || k_msgq_num_used_get(&history);
	k_spin_unlock(&lock, key);
	if (more) {
		k_work_submit(&input_work);
	}
	(void)ui_hardware_led(0, bt.phase == BLUETOOTH_AWAKE);
	(void)ui_hardware_led(1, bt.phase == BLUETOOTH_PREPARING ||
					 bt.phase == BLUETOOTH_ENABLING ||
					 bt.phase == BLUETOOTH_STOPPING);
}
static int initialize(void)
{
	gestures_init(&gestures, emit);
	k_spinlock_key_t key = k_spin_lock(&lock);
	boot_time = k_uptime_get();
	initialized = true;
	k_spin_unlock(&lock, key);
	k_work_submit(&input_work);
	return 0;
}

SYS_INIT(initialize, APPLICATION, 90);
