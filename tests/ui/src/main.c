/* SPDX-License-Identifier: Apache-2.0 */
#include "private.h"
#include <cwbeat/bluetooth.h>
#include <cwbeat/power.h>
#include <zephyr/ztest.h>
ZBUS_CHAN_DEFINE(bluetooth_states, struct bluetooth_state, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(power_states, struct power_state, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
struct action {
	bool power;
	int command;
};
K_MSGQ_DEFINE(actions, sizeof(struct action), 16, 4);
static bool leds[2];
int ui_hardware_led(unsigned channel, bool on)
{
	leds[channel] = on;
	return 0;
}
int bluetooth_request(enum bluetooth_command_kind command, uint32_t *id)
{
	*id = 1;
	struct action action = {false, command};
	return k_msgq_put(&actions, &action, K_NO_WAIT);
}
int power_request(enum power_command_kind command, uint32_t *id)
{
	*id = 1;
	struct action action = {true, command};
	return k_msgq_put(&actions, &action, K_NO_WAIT);
}
K_SEM_DEFINE(block_started, 0, 1);
K_SEM_DEFINE(unblock, 0, 1);
static void block(struct k_work *work)
{
	ARG_UNUSED(work);
	k_sem_give(&block_started);
	k_sem_take(&unblock, K_FOREVER);
}
K_WORK_DEFINE(blocker, block);
static void await(bool power, int command)
{
	struct action action;
	zassert_ok(k_msgq_get(&actions, &action, K_MSEC(500)));
	zassert_equal(action.power, power);
	zassert_equal(action.command, command);
}
static void edge(enum cwbeat_button button, bool down)
{
	ui_input_event(button, down, k_uptime_get_32());
}
ZTEST(ui, test_deadline_and_overflow_preserve_held_resync)
{
	struct bluetooth_state bt = {.phase = BLUETOOTH_AWAKE};
	struct power_state board = {.intent = POWER_ON};
	zassert_ok(zbus_chan_pub(&bluetooth_states, &bt, K_NO_WAIT));
	zassert_ok(zbus_chan_pub(&power_states, &board, K_NO_WAIT));
	k_sleep(K_MSEC(450));
	zassert_true(leds[0]);
	edge(BUTTON_PLAY, true);
	edge(BUTTON_PLAY, false);
	await(false, BLUETOOTH_PLAY_PAUSE); /* No input arrives during the click-window deadline. */
	edge(BUTTON_UP, true);
	await(false, BLUETOOTH_VOLUME_UP);
	edge(BUTTON_UP, false);
	k_sleep(K_MSEC(20));
	zassert_equal(k_msgq_num_used_get(&actions), 0);
	k_work_submit(&blocker);
	zassert_ok(k_sem_take(&block_started, K_MSEC(100)));
	for (unsigned i = 0; i < 40; ++i) {
		edge(BUTTON_BT, i % 2 == 0);
	}
	edge(BUTTON_BT, true); /* overflow ends held: require a real release before rearming */
	k_sem_give(&unblock);
	k_sleep(K_MSEC(20));
	edge(BUTTON_BT, false);
	k_sleep(K_MSEC(20));
	zassert_equal(k_msgq_num_used_get(&actions), 0);
	edge(BUTTON_BT, true);
	edge(BUTTON_BT, false);
	await(false, BLUETOOTH_DISCOVER);
	/* Mode changes discard incomplete clicks and preserve live held levels. */
	edge(BUTTON_PLAY, true);
	edge(BUTTON_PLAY, false);
	board.intent = POWER_OFF;
	bt.phase = BLUETOOTH_STOPPED;
	zassert_ok(zbus_chan_pub(&bluetooth_states, &bt, K_NO_WAIT));
	zassert_ok(zbus_chan_pub(&power_states, &board, K_NO_WAIT));
	k_sleep(K_MSEC(350));
	zassert_equal(k_msgq_num_used_get(&actions), 0);
	edge(BUTTON_POWER, true);
	await(true, POWER_ON);
	edge(BUTTON_POWER, false);
	zassert_false(leds[0]);
}
ZTEST_SUITE(ui, NULL, NULL, NULL, NULL, NULL);
