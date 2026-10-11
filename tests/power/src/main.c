/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include "power.c"
#include "autostart.c"
ZBUS_CHAN_DEFINE(bluetooth_commands, struct bluetooth_command, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(bluetooth_results, struct bluetooth_result, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(bluetooth_states, struct bluetooth_state, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_states, struct stock_peer_state, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(audio_states, struct audio_state, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
static bool boot_requested, console_done;
static unsigned on_count, terminal_count, bt_count, other_count;
static enum bluetooth_command_kind last_bt;
static uint32_t fake_bt_id;
static struct k_work other;
static void other_work(struct k_work *w)
{
	ARG_UNUSED(w);
	++other_count;
}
bool power_hardware_boot_requested(void)
{
	return boot_requested;
}
void power_hardware_on(void)
{
	++on_count;
}
bool power_hardware_console_done(void)
{
	return console_done;
}
int power_hardware_terminal(enum power_command_kind command)
{
	ARG_UNUSED(command);
	++terminal_count;
	return 0;
}
int bluetooth_request(enum bluetooth_command_kind command, uint32_t *id)
{
	last_bt = command;
	++bt_count;
	*id = ++fake_bt_id;
	return 0;
}
static void before(void *unused)
{
	ARG_UNUSED(unused);
	struct k_work_sync sync;
	k_work_cancel_delayable_sync(&deadline, &sync);
	k_work_flush(&work, &sync);
	head = count = credits = next_id = reserved_id = bt_id = bt_session = result_head =
		result_count = 0;
	terminal_credit = reserved = received = dirty = bt_result_pending = stopped_this_boot =
		false;
	phase = IDLE;
	state = (struct power_state){.off_pending = true, .intent = POWER_STOP};
	peer = (struct stock_peer_state){0};
	audio = (struct audio_state){0};
	console_done = false;
	boot_requested = false;
	on_count = terminal_count = bt_count = other_count = fake_bt_id = 0;
	k_work_init(&other, other_work);
}
static void wait_bt(void)
{
	for (unsigned i = 0; i < 100 && !bt_count; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_equal(bt_count, 1);
	/* Await the caller's correlation bookkeeping, too. */
	k_sleep(K_MSEC(2));
}
static void result(bool safe, int status, uint32_t session)
{
	if (safe) {
		struct stock_peer_state ps = {.session = session, .phase = STOCK_PEER_HELD};
		struct audio_state as = {
			.session = session, .quiescent = true, .raises_revoked = true};
		zassert_ok(zbus_chan_pub(&stock_peer_states, &ps, K_NO_WAIT));
		zassert_ok(zbus_chan_pub(&audio_states, &as, K_NO_WAIT));
	}
	struct bluetooth_result br = {
		.id = fake_bt_id, .session = session, .command = last_bt, .status = status};
	zassert_ok(zbus_chan_pub(&bluetooth_results, &br, K_NO_WAIT));
}
ZTEST(power, test_no_boot_power_press_suppresses_autostart)
{
	zassert_ok(power_init());
	k_sleep(K_MSEC(5));
	zassert_equal(power_snapshot().intent, POWER_OFF);
	power_autostart();
	k_sleep(K_MSEC(10));
	zassert_equal(bt_count, 0);
	zassert_equal(on_count, 0);
	uint32_t id;
	zassert_ok(power_request(POWER_ON, &id));
	wait_bt();
	zassert_equal(on_count, 1);
	zassert_equal(last_bt, BLUETOOTH_ENABLE);
	result(false, 0, 1);
	k_sleep(K_MSEC(10));
	zassert_false(power_snapshot().transitioning);
}
ZTEST(power, test_boot_power_press_allows_autostart)
{
	boot_requested = true;
	zassert_ok(power_init());
	k_sleep(K_MSEC(5));
	zassert_false(power_snapshot().off_pending);
	power_autostart();
	wait_bt();
	zassert_equal(on_count, 1);
	zassert_equal(last_bt, BLUETOOTH_ENABLE);
}
ZTEST(power, test_recovery_before_first_session)
{
	console_done = true;
	zassert_ok(power_request(POWER_RECOVERY, NULL));
	wait_bt();
	result(false, 0, 0);
	for (unsigned i = 0; i < 400 && !terminal_count; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_equal(terminal_count, 1);
	zassert_equal(power_snapshot().error, 0);
}
ZTEST(power, test_reset_before_first_session)
{
	console_done = true;
	zassert_ok(power_request(POWER_RESET, NULL));
	wait_bt();
	result(false, 0, 0);
	for (unsigned i = 0; i < 400 && !terminal_count; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_equal(terminal_count, 1);
	zassert_equal(power_snapshot().error, 0);
}
ZTEST(power, test_session_zero_failure_cannot_authorize_terminal_action)
{
	console_done = true;
	zassert_ok(power_request(POWER_RECOVERY, NULL));
	wait_bt();
	result(false, -EIO, 0);
	k_sleep(K_MSEC(1100));
	zassert_equal(terminal_count, 0);
	zassert_equal(power_snapshot().error, -EIO);
}
ZTEST(power, test_session_zero_cannot_bypass_prepared_peers)
{
	console_done = true;
	zassert_ok(power_request(POWER_RECOVERY, NULL));
	wait_bt();
	struct stock_peer_state started = {.session = 1, .phase = STOCK_PEER_PREPARED};
	zassert_ok(zbus_chan_pub(&stock_peer_states, &started, K_NO_WAIT));
	result(false, 0, 0);
	k_sleep(K_MSEC(1100));
	zassert_equal(terminal_count, 0);
	zassert_equal(power_snapshot().error, -EIO);
}
ZTEST(power, test_safe_shutdown_grace_and_nonblocking_drain)
{
	zassert_ok(power_request(POWER_OFF, NULL));
	wait_bt();
	result(true, 0, 1);
	k_sleep(K_MSEC(100));
	zassert_equal(terminal_count, 0);
	k_work_submit(&other);
	k_sleep(K_MSEC(10));
	zassert_equal(other_count, 1);
	k_sleep(K_MSEC(200));
	zassert_equal(terminal_count, 0);
	console_done = true;
	for (unsigned i = 0; i < 100 && !terminal_count; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_equal(terminal_count, 1);
	zassert_false(power_snapshot().transitioning);
}
ZTEST(power, test_failed_hold_prevents_terminal_action)
{
	zassert_ok(power_request(POWER_RECOVERY, NULL));
	wait_bt();
	result(false, -EIO, 1);
	k_sleep(K_MSEC(1100));
	zassert_equal(terminal_count, 0);
	zassert_equal(power_snapshot().error, -EIO);
	zassert_false(power_snapshot().transitioning);
}
ZTEST(power, test_stale_session_safe_proof_cannot_authorize)
{
	zassert_ok(power_request(POWER_RESET, NULL));
	wait_bt();
	struct stock_peer_state stale_peer = {.session = 99, .phase = STOCK_PEER_HELD};
	struct audio_state stale_audio = {.session = 99, .quiescent = true, .raises_revoked = true};
	zassert_ok(zbus_chan_pub(&stock_peer_states, &stale_peer, K_NO_WAIT));
	zassert_ok(zbus_chan_pub(&audio_states, &stale_audio, K_NO_WAIT));
	struct bluetooth_result corrected = {
		.id = fake_bt_id, .session = 1, .command = BLUETOOTH_DISABLE};
	/* Replace old proof with a matched result before processing on shared queue. */
	zassert_ok(zbus_chan_pub(&bluetooth_results, &corrected, K_NO_WAIT));
	k_sleep(K_MSEC(1100));
	zassert_equal(terminal_count, 0);
}
ZTEST(power, test_native_error_with_safe_resources_allows_off)
{
	console_done = true;
	zassert_ok(power_request(POWER_OFF, NULL));
	wait_bt();
	result(true, -ETIMEDOUT, 1);
	for (unsigned i = 0; i < 400 && !terminal_count; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_equal(terminal_count, 1);
	zassert_equal(power_snapshot().error, -ETIMEDOUT);
}
ZTEST(power, test_console_drain_deadline_bounds_wait)
{
	zassert_ok(power_request(POWER_RESET, NULL));
	wait_bt();
	result(true, 0, 1);
	int64_t start = k_uptime_get();
	for (unsigned i = 0; i < 2600 && !terminal_count; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_equal(terminal_count, 1);
	zassert_true(k_uptime_get() - start >= 2250);
}
static void after(void *unused)
{
	ARG_UNUSED(unused);
	struct k_work_sync sync;
	k_work_cancel_delayable_sync(&deadline, &sync);
	k_work_flush(&work, &sync);
}
ZTEST(power, test_off_preempts_active_on)
{
	zassert_ok(power_request(POWER_ON, NULL));
	wait_bt();
	zassert_ok(power_request(POWER_OFF, NULL));
	for (unsigned i = 0; i < 100 && bt_count < 2; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_equal(bt_count, 2);
	zassert_equal(last_bt, BLUETOOTH_DISABLE);
	console_done = true;
	result(true, 0, 1);
	for (unsigned i = 0; i < 400 && !terminal_count; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_equal(terminal_count, 1);
}
ZTEST(power, test_failed_publication_leaves_power_intent_unchanged)
{
	state.off_pending = false;
	state.intent = POWER_ON;
	zassert_ok(zbus_chan_claim(&power_commands, K_NO_WAIT));
	zassert_not_equal(power_request(POWER_OFF, NULL), 0);
	zassert_false(power_snapshot().off_pending);
	zassert_equal(power_snapshot().intent, POWER_ON);
	zassert_equal(credits, 0);
	zassert_false(terminal_credit);
	zassert_ok(zbus_chan_finish(&power_commands));
}
static struct k_sem blocker_started, blocker_resume;
static struct k_work blocker;
static struct power_result observed_results[3];
static unsigned observed_count;
static void observe_result(const struct zbus_channel *channel)
{
	if (observed_count < ARRAY_SIZE(observed_results)) {
		observed_results[observed_count++] =
			*(const struct power_result *)zbus_chan_const_msg(channel);
	}
}
ZBUS_LISTENER_DEFINE(result_observer, observe_result);
ZBUS_CHAN_ADD_OBS(power_results, result_observer, 5);
static void block_queue(struct k_work *w)
{
	ARG_UNUSED(w);
	k_sem_give(&blocker_started);
	k_sem_take(&blocker_resume, K_FOREVER);
}
ZTEST(power, test_stop_cancels_all_queued_on_and_prevents_restart)
{
	observed_count = 0;
	k_sem_init(&blocker_started, 0, 1);
	k_sem_init(&blocker_resume, 0, 1);
	k_work_init(&blocker, block_queue);
	k_work_submit(&blocker);
	zassert_ok(k_sem_take(&blocker_started, K_MSEC(100)));
	uint32_t first, second, stop;
	zassert_ok(power_request(POWER_ON, &first));
	zassert_ok(power_request(POWER_ON, &second));
	zassert_ok(power_request(POWER_STOP, &stop));
	k_sem_give(&blocker_resume);
	wait_bt();
	zassert_equal(last_bt, BLUETOOTH_DISABLE);
	zassert_equal(on_count, 0);
	result(true, 0, 1);
	for (unsigned i = 0; i < 100 && observed_count < 3; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_equal(observed_count, 3);
	zassert_equal(observed_results[0].id, first);
	zassert_equal(observed_results[0].status, -ECANCELED);
	zassert_equal(observed_results[1].id, second);
	zassert_equal(observed_results[1].status, -ECANCELED);
	zassert_equal(observed_results[2].id, stop);
	zassert_equal(observed_results[2].status, 0);
	zassert_equal(power_request(POWER_ON, NULL), -ENOTSUP);
	k_sleep(K_MSEC(10));
	zassert_equal(on_count, 0);
	zassert_equal(bt_count, 1);
}
ZTEST_SUITE(power, NULL, NULL, before, after, NULL);
