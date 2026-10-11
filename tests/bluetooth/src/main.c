/* SPDX-License-Identifier: Apache-2.0 */
#include <cwbeat/bluetooth.h>
#include <cwbeat/audio.h>
#include <cwbeat/stock_peer.h>
#include <zephyr/ztest.h>
#include <errno.h>
ZBUS_CHAN_DEFINE(stock_peer_requests, struct stock_peer_request, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_bt_results, struct stock_peer_result, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_audio_results, struct stock_peer_result, NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_states, struct stock_peer_state, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(audio_commands, struct audio_command, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(audio_results, struct audio_result, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(audio_states, struct audio_state, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
static struct stock_peer_state peer;
static struct audio_state audio;
static enum stock_peer_operation operations[32];
static unsigned operation_count;
extern unsigned bluetooth_test_peer_completion_count(void);
extern unsigned bluetooth_test_audio_completion_count(void);
static bool suspend_results;
static bool transport_full;
static struct stock_peer_request suspended;
#ifdef BT_TEST_DISABLE_DURING_CLEANUP
static bool suspend_hold;
static struct stock_peer_request suspended_hold;
K_SEM_DEFINE(hold_started, 0, 1);
#endif
K_MSGQ_DEFINE(results, sizeof(struct bluetooth_result), 8, 4);
ZBUS_SUBSCRIBER_DEFINE(late_diagnostic, 1);
ZBUS_CHAN_ADD_OBS(stock_peer_bt_results, late_diagnostic, 99);
K_SEM_DEFINE(block_started, 0, 1);
K_SEM_DEFINE(unblock, 0, 1);
static void block_workqueue(struct k_work *work)
{
	ARG_UNUSED(work);
	k_sem_give(&block_started);
	k_sem_take(&unblock, K_FOREVER);
}
K_WORK_DEFINE(blocker, block_workqueue);
static void capture(const struct zbus_channel *channel)
{
	zassert_ok(k_msgq_put(&results, zbus_chan_const_msg(channel), K_NO_WAIT));
}
ZBUS_LISTENER_DEFINE(capture_results, capture);
ZBUS_CHAN_ADD_OBS(bluetooth_results, capture_results, 0);
static void publish_peer(void)
{
	zassert_ok(zbus_chan_pub(&stock_peer_states, &peer, K_NO_WAIT));
}
static void publish_audio(void)
{
	zassert_ok(zbus_chan_pub(&audio_states, &audio, K_NO_WAIT));
}
int stock_peer_request(const struct stock_peer_request *request)
{
	if (transport_full && request->operation >= STOCK_PEER_FEEDBACK_DISABLE) {
		return -ENOSPC;
	}
	zassert_true(operation_count < ARRAY_SIZE(operations));
	operations[operation_count++] = request->operation;
	switch (request->operation) {
	case STOCK_PEER_PREPARE:
		peer.session = request->token.session;
		peer.phase = STOCK_PEER_PREPARED;
		publish_peer();
		break;
	case STOCK_PEER_RELEASE:
		zassert_true(audio.controls_ready);
		peer.phase = STOCK_PEER_RELEASED;
		peer.readiness = 1;
		publish_peer();
		break;
	case STOCK_PEER_WAKE:
		peer.readiness = 2;
		publish_peer();
		break;
	case STOCK_PEER_SHUTDOWN:
		zassert_true(peer.admission_closed);
		zassert_true(audio.raises_revoked && audio.callbacks_settled);
#ifndef BT_TEST_SHUTDOWN_TIMEOUT
		peer.readiness = 1;
		publish_peer();
#endif
		break;
	case STOCK_PEER_HOLD:
#ifdef BT_TEST_DISABLE_DURING_CLEANUP
		if (suspend_hold) {
			suspended_hold = *request;
			k_sem_give(&hold_started);
			return 0;
		}
#endif
		peer.phase = STOCK_PEER_HELD;
		publish_peer();
		break;
	default:
		break;
	}
	if (suspend_results && request->operation >= STOCK_PEER_FEEDBACK_DISABLE) {
		suspended = *request;
		return 0;
	}
	struct stock_peer_result result = {.token = request->token,
					   .operation = request->operation};
	zassert_ok(zbus_chan_pub(&stock_peer_bt_results, &result, K_NO_WAIT));
	return 0;
}
int stock_peer_close_admission(uint32_t session)
{
	zassert_equal(session, peer.session);
	peer.admission_closed = true;
	publish_peer();
	return 0;
}
int audio_revoke_raises(uint32_t session)
{
	zassert_equal(session, audio.session);
	audio.raises_revoked = true;
	audio.callbacks_settled = true;
	publish_audio();
	return 0;
}
int audio_request(const struct audio_command *command)
{
	audio.session = command->session;
	if (command->operation == AUDIO_PREPARE) {
		audio.controls_ready = audio.mode3_ready = true;
	} else {
		zassert_equal(peer.phase, STOCK_PEER_HELD);
		audio.quiescent = true;
	}
	publish_audio();
	struct audio_result result = {command->session, command->id, command->operation, 0};
	zassert_ok(zbus_chan_pub(&audio_results, &result, K_NO_WAIT));
	return 0;
}
static struct bluetooth_result wait_result(uint32_t id, int status)
{
	struct bluetooth_result result;
	zassert_ok(k_msgq_get(&results, &result, K_MSEC(1500)), "id %u", id);
	zassert_equal(result.id, id);
	zassert_equal(result.status, status);
	return result;
}
static uint32_t request(enum bluetooth_command_kind command)
{
	uint32_t id = 0;
	zassert_ok(bluetooth_request(command, &id));
	zassert_not_equal(id, 0);
	return id;
}
#ifdef BT_TEST_DISABLE_DURING_CLEANUP
ZTEST(bluetooth, test_disable_waits_for_automatic_cleanup)
{
	zassert_ok(zbus_obs_set_enable(&late_diagnostic, false));
	uint32_t enable = request(BLUETOOTH_ENABLE);
	wait_result(enable, 0);
	zassert_equal(bluetooth_snapshot().phase, BLUETOOTH_AWAKE);

	/* A peer fault starts cleanup without an external DISABLE owner. Keep the
	 * hold in flight so a later power-off must join that same cleanup. */
	suspend_hold = true;
	peer.phase = STOCK_PEER_FAILED;
	peer.error = -ETIMEDOUT;
	publish_peer();
	zassert_ok(k_sem_take(&hold_started, K_MSEC(500)));
	zassert_equal(bluetooth_snapshot().phase, BLUETOOTH_STOPPING);
	unsigned cleanup_operations = operation_count;
	uint32_t disable = request(BLUETOOTH_DISABLE);
	struct bluetooth_result early = {0};
	int status = k_msgq_get(&results, &early, K_MSEC(20));
	zassert_not_equal(status, 0,
			  "DISABLE completed before cleanup: id=%u status=%d", early.id,
			  early.status);
	zassert_equal(bluetooth_snapshot().phase, BLUETOOTH_STOPPING);
	zassert_equal(bluetooth_snapshot().error, -ETIMEDOUT);
	zassert_equal(operation_count, cleanup_operations);
	zassert_false(audio.quiescent);
	uint32_t rejected = 0;
	zassert_equal(bluetooth_request(BLUETOOTH_DISABLE, &rejected), -ENOSPC);
	zassert_equal(rejected, 0);

	peer.phase = STOCK_PEER_HELD;
	peer.error = 0;
	publish_peer();
	struct stock_peer_result held = {.token = suspended_hold.token,
					.operation = STOCK_PEER_HOLD};
	zassert_ok(zbus_chan_pub(&stock_peer_bt_results, &held, K_NO_WAIT));
	wait_result(disable, -ETIMEDOUT);
	zassert_equal(bluetooth_snapshot().phase, BLUETOOTH_STOPPED);
	zassert_true(audio.quiescent);
}
#else
static void settle(void)
{
	k_sleep(K_MSEC(20));
}
ZTEST(bluetooth, test_session_order_guards_stale_state_and_reserved_disable)
{
	zassert_ok(zbus_obs_set_enable(&late_diagnostic, false));
	uint32_t id = request(BLUETOOTH_ENABLE);
	wait_result(id, 0);
	zassert_equal(bluetooth_snapshot().phase, BLUETOOTH_AWAKE);
	zassert_equal(operations[0], STOCK_PEER_PREPARE);
	zassert_equal(operations[1], STOCK_PEER_RELEASE);
	zassert_equal(operations[2], STOCK_PEER_FEEDBACK_DISABLE);
	zassert_equal(operations[operation_count - 1], STOCK_PEER_WAKE);
	unsigned count = operation_count;
	id = request(BLUETOOTH_PLAY_PAUSE);
	wait_result(id, -ENOTCONN);
	zassert_equal(operation_count, count);
	peer.connection = 2;
	peer.playing = 1;
	publish_peer();
	settle();
	id = request(BLUETOOTH_PLAY_PAUSE);
	wait_result(id, 0);
	zassert_equal(operations[operation_count - 1], STOCK_PEER_PAUSE);
	peer.auxiliary = 2;
	publish_peer();
	settle();
	id = request(BLUETOOTH_NEXT);
	wait_result(id, -ENOTSUP);
	peer.auxiliary = 0;
	publish_peer();
	settle();
	struct stock_peer_state stale = peer;
	stale.session = 99;
	stale.connection = 0;
	zassert_ok(zbus_chan_pub(&stock_peer_states, &stale, K_NO_WAIT));
	settle();
	zassert_equal(bluetooth_snapshot().connection, 2);
	id = request(BLUETOOTH_VOLUME_UP);
	wait_result(id, 0);
	zassert_equal(operations[operation_count - 1], STOCK_PEER_VOLUME_UP);
	/* A blocked FIFO head expires by its original submission timestamp. */
	transport_full = true;
	id = request(BLUETOOTH_NEXT);
	wait_result(id, -ETIMEDOUT);
	transport_full = false;
	/* Fill five normal completion reservations with one committed RPC plus four queued
	 * commands. */
	suspend_results = true;
	uint32_t ids[5];
	ids[0] = request(BLUETOOTH_NEXT);
	settle();
	k_work_submit(&blocker);
	zassert_ok(k_sem_take(&block_started, K_MSEC(100)));
	zassert_ok(zbus_obs_set_enable(&late_diagnostic, true));
	struct stock_peer_result replay = {.token = suspended.token,
					   .operation = suspended.operation};
	zassert_ok(zbus_chan_pub(&stock_peer_bt_results, &replay, K_NO_WAIT));
	for (unsigned i = 0; i < 32; ++i) {
		zassert_not_equal(zbus_chan_pub(&stock_peer_bt_results, &replay, K_NO_WAIT), 0);
	}
	/* Distinct reserved lanes complete in reverse token order. They cannot be
	 * deduplicated by one global high-water mark. */
	replay.operation = STOCK_PEER_HOLD;
	replay.token.id = suspended.token.id - 1;
	zassert_not_equal(zbus_chan_pub(&stock_peer_bt_results, &replay, K_NO_WAIT), 0);
	replay.operation = STOCK_PEER_SHUTDOWN;
	replay.token.id = suspended.token.id - 2;
	zassert_not_equal(zbus_chan_pub(&stock_peer_bt_results, &replay, K_NO_WAIT), 0);
	/* Audio command completion retries likewise preserve only one occurrence. */
	struct audio_result audio_replay = {1, suspended.token.id - 1, AUDIO_PREPARE, 0};
	for (unsigned i = 0; i < 32; ++i) {
		zassert_ok(zbus_chan_pub(&audio_results, &audio_replay, K_NO_WAIT));
	}
	zassert_equal(bluetooth_test_peer_completion_count(), 3);
	zassert_equal(bluetooth_test_audio_completion_count(), 1);
#ifdef BT_TEST_COMPLETION_OVERFLOW
	/* Violating the adapter's three-lane reservation guarantee must fail closed. */
	replay.operation = STOCK_PEER_NEXT;
	for (unsigned i = 1; i <= 9; ++i) {
		replay.token.id = suspended.token.id + i;
		zassert_not_equal(zbus_chan_pub(&stock_peer_bt_results, &replay, K_NO_WAIT), 0);
	}
#endif
	zassert_ok(zbus_obs_set_enable(&late_diagnostic, false));
	for (unsigned i = 1; i < ARRAY_SIZE(ids); ++i) {
		ids[i] = request(BLUETOOTH_NEXT);
	}
	uint32_t rejected;
	zassert_equal(bluetooth_request(BLUETOOTH_NEXT, &rejected), -ENOSPC);
	uint32_t stop = request(BLUETOOTH_DISABLE);
	suspend_results = false;
	k_sem_give(&unblock);
	for (unsigned i = 0; i < ARRAY_SIZE(ids); ++i) {
		wait_result(ids[i], -ECANCELED);
	}
#if defined(BT_TEST_COMPLETION_OVERFLOW)
	wait_result(stop, -ENOBUFS);
#elif defined(BT_TEST_SHUTDOWN_TIMEOUT)
	wait_result(stop, -ETIMEDOUT);
	unsigned shutdowns = 0;
	for (unsigned i = 0; i < operation_count; ++i) {
		shutdowns += operations[i] == STOCK_PEER_SHUTDOWN;
	}
	zassert_equal(shutdowns, 5);
#else
	wait_result(stop, 0);
#endif
	zassert_equal(bluetooth_snapshot().phase, BLUETOOTH_STOPPED);
	zassert_true(audio.quiescent);
	struct stock_peer_result late = {.token = suspended.token,
					 .operation = suspended.operation};
	zassert_ok(zbus_chan_pub(&stock_peer_bt_results, &late, K_NO_WAIT));
	settle();
	zassert_equal(k_msgq_num_used_get(&results), 0);
	id = request(BLUETOOTH_ENABLE);
	wait_result(id, -EALREADY);
}
#endif
ZTEST_SUITE(bluetooth, NULL, NULL, NULL, NULL, NULL);
