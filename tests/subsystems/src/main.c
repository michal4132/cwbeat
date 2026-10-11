/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <cwbeat/audio.h>
#include <cwbeat/bluetooth.h>
#include <cwbeat/stock_peer.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
ZBUS_CHAN_DEFINE(stock_peer_requests, struct stock_peer_request, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_bt_results, struct stock_peer_result, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_audio_results, struct stock_peer_result, NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_states, struct stock_peer_state, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_callbacks, struct stock_peer_callback, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_callback_responses, struct stock_peer_callback_response, NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));
K_MSGQ_DEFINE(fake_requests, sizeof(struct stock_peer_request), 8, 4);
static struct stock_peer_state fake_peer;
static struct k_work fake_work;
static atomic_t raw_requests, dsp_requests, raises, held, closed, callback_response;
static unsigned callback_token;
static unsigned highs_at_close;
static bool pin16;
static void dispatch(struct k_work *work)
{
	ARG_UNUSED(work);
	struct stock_peer_request r;
	while (!k_msgq_get(&fake_requests, &r, K_NO_WAIT)) {
		switch (r.operation) {
		case STOCK_PEER_PREPARE:
			fake_peer = (struct stock_peer_state){
				.session = 1, .phase = STOCK_PEER_PREPARED, .board_type = 1};
			break;
		case STOCK_PEER_RELEASE:
			fake_peer.phase = STOCK_PEER_RELEASED;
			fake_peer.readiness = 1;
			break;
		case STOCK_PEER_WAKE:
			fake_peer.readiness = 2;
			break;
		case STOCK_PEER_SHUTDOWN:
			fake_peer.readiness = 1;
			break;
		case STOCK_PEER_HOLD:
			fake_peer.session = 1;
			fake_peer.phase = STOCK_PEER_HELD;
			fake_peer.admission_closed = true;
			atomic_set(&held, 1);
			break;
		case STOCK_PEER_RAW_ACTIVE:
			atomic_inc(&raw_requests);
			break;
		case STOCK_PEER_DSP_PROFILE:
			atomic_inc(&dsp_requests);
			break;
		default:
			break;
		}
		zassert_ok(zbus_chan_pub(&stock_peer_states, &fake_peer, K_NO_WAIT));
		struct stock_peer_result result = {r.token, r.operation, 0};
		zassert_ok(zbus_chan_pub(r.token.owner == STOCK_PEER_OWNER_AUDIO
						 ? &stock_peer_audio_results
						 : &stock_peer_bt_results,
					 &result, K_NO_WAIT));
	}
}
int stock_peer_request(const struct stock_peer_request *r)
{
	if (atomic_get(&closed) && r->operation > STOCK_PEER_SHUTDOWN) {
		return -ESHUTDOWN;
	}
	int rc = k_msgq_put(&fake_requests, r, K_NO_WAIT);
	if (!rc) {
		k_work_submit(&fake_work);
	}
	return rc;
}
int stock_peer_close_admission(uint32_t session)
{
	zassert_equal(session, 1);
	highs_at_close = atomic_get(&raises);
	atomic_set(&closed, 1);
	return 0;
}
struct stock_peer_state stock_peer_snapshot(void)
{
	return fake_peer;
}
int stock_peer_callback_respond(const struct stock_peer_callback_response *r)
{
	zassert_equal(r->session, 1);
	zassert_equal(r->token, callback_token);
	atomic_set(&callback_response, r->token);
	return 0;
}
int audio_hardware_prepare(void)
{
	pin16 = false;
	return 0;
}
int audio_hardware_write(unsigned pin, bool high)
{
	if (high) {
		zassert_false(atomic_get(&closed), "High after transport closure/raise barrier");
		atomic_inc(&raises);
	}
	if (pin == 16) {
		pin16 = high;
	}
	return 0;
}
static void callback(enum stock_peer_callback_kind kind, uint8_t mode)
{
	struct stock_peer_callback cb = {
		.session = 1, .token = ++callback_token, .kind = kind, .mode = mode};
	zassert_ok(zbus_chan_pub(&stock_peer_callbacks, &cb, K_NO_WAIT));
}
static void wait_audio(bool (*predicate)(struct audio_state), int milliseconds)
{
	int64_t end = k_uptime_get() + milliseconds;
	while (!predicate(audio_snapshot()) && k_uptime_get() < end) {
		k_sleep(K_MSEC(5));
	}
	zassert_true(predicate(audio_snapshot()));
}
static bool controls(struct audio_state s)
{
	return s.controls_ready;
}
static bool mode3(struct audio_state s)
{
	return s.mode3_ready;
}
#ifdef TEST_COMPLETE_STARTUP
static bool mode4(struct audio_state s)
{
	return s.mode4_ready;
}
static bool initialized_codec(struct audio_state s)
{
	return s.codec_initialized;
}
#endif
static bool stopped_audio(struct audio_state s)
{
	return s.quiescent;
}
ZTEST(subsystems, test_real_workqueue_start_and_stop)
{
	k_work_init(&fake_work, dispatch);
	uint32_t id;
	zassert_ok(bluetooth_request(BLUETOOTH_ENABLE, &id));
	wait_audio(controls, 1000);
	callback(STOCK_PEER_POWER_MODE, 3);
	k_sleep(K_MSEC(100));
	zassert_false(audio_snapshot().mode3_ready);
	wait_audio(mode3, 1000);
	int64_t end = k_uptime_get() + 1000;
	while (!atomic_get(&raw_requests) && k_uptime_get() < end) {
		k_sleep(K_MSEC(5));
	}
	zassert_equal(atomic_get(&raw_requests), 1);
	zassert_equal(bluetooth_snapshot().phase, BLUETOOTH_AWAKE);
	callback(STOCK_PEER_POWER_MODE, 4);
	k_sleep(K_MSEC(50));
	zassert_false(audio_snapshot().mode4_ready);
	zassert_false(pin16);
#ifdef TEST_COMPLETE_STARTUP
	wait_audio(mode4, 1000);
	zassert_true(pin16);
	callback(STOCK_PEER_CODEC_INIT, 0);
	wait_audio(initialized_codec, 1000);
	zassert_equal(atomic_get(&dsp_requests), 1);
	zassert_true(audio_snapshot().ready);
#endif
	zassert_ok(bluetooth_request(BLUETOOTH_DISABLE, &id));
	wait_audio(stopped_audio, 2000);
	zassert_true(atomic_get(&held));
	zassert_false(pin16);
	zassert_true(audio_snapshot().raises_revoked);
	zassert_equal(atomic_get(&raises), highs_at_close);
	zassert_equal(bluetooth_snapshot().phase, BLUETOOTH_STOPPED);
#ifndef TEST_COMPLETE_STARTUP
	zassert_equal(atomic_get(&raises), 0);
	zassert_false(audio_snapshot().ready);
#endif
}
ZTEST_SUITE(subsystems, NULL, NULL, NULL, NULL, NULL);
