/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
static bool real_queue;
static int manual_submit(struct k_work *w)
{
	return real_queue ? k_work_submit(w) : 0;
}
static int manual_reschedule(struct k_work_delayable *w, k_timeout_t t)
{
	return real_queue ? k_work_reschedule(w, t) : 0;
}
#define k_work_submit     manual_submit
#define k_work_reschedule manual_reschedule
#include "audio.c"
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
ZBUS_SUBSCRIBER_DEFINE_WITH_ENABLE(late_subscriber, 1, false);
ZBUS_CHAN_ADD_OBS(stock_peer_callbacks, late_subscriber, 9);
static unsigned writes, raises, requests, responses;
static unsigned pins[16];
static bool values[16];
static struct stock_peer_request last_request;
static struct stock_peer_callback_response last_response;
static int response_error;
int audio_hardware_prepare(void)
{
	return 0;
}
int audio_hardware_write(unsigned p, bool high)
{
	if (writes < ARRAY_SIZE(pins)) {
		pins[writes] = p;
		values[writes] = high;
	}
	++writes;
	if (high) {
		++raises;
	}
	return 0;
}
int stock_peer_request(const struct stock_peer_request *r)
{
	last_request = *r;
	++requests;
	return 0;
}
int stock_peer_callback_respond(const struct stock_peer_callback_response *r)
{
	last_response = *r;
	if (response_error) {
		return response_error;
	}
	++responses;
	return 0;
}
static void before(void *unused)
{
	ARG_UNUSED(unused);
	real_queue = false;
	zassert_ok(zbus_obs_set_enable(&late_subscriber, false));
	const struct zbus_channel *ignored;
	while (!zbus_sub_wait(&late_subscriber, &ignored, K_NO_WAIT)) {
	}
	struct k_work_sync sync;
	k_work_cancel_delayable_sync(&deadline, &sync);
	k_work_flush(&work, &sync);
	state = (struct audio_state){.session = 1,
				     .controls_ready = true,
				     .mode3_ready = true,
				     .mode4_ready = true,
				     .callbacks_settled = true};
	peer = (struct stock_peer_state){
		.session = 1, .phase = STOCK_PEER_RELEASED, .board_type = 1};
	command_head = command_count = command_reserved = callback_head = callback_count =
		result_count = 0;
	codec_head = codec_count = 0;
	last_callback_session = last_callback_token = 0;
	publication_inflight = false;
	completion_pending = command_busy = state_dirty = false;
	response_pending = mode_pending = response_enqueues = false;
	codec_running = codec_second = shutdown_reserved = request_outstanding = raw_sent =
		raw_done = awake_seen = false;
	mode_phase = 0;
	next_id = 0;
	writes = raises = requests = responses = 0;
	response_error = 0;
}
static void callback(enum stock_peer_callback_kind kind)
{
	struct stock_peer_callback c = {.session = 1, .token = 17, .kind = kind};
	process_callback(&c, k_uptime_get());
	run(NULL);
}
ZTEST(audio, test_revocation_cancels_delayed_high)
{
	callback(STOCK_PEER_CODEC_ENABLE);
	run(NULL);
	zassert_equal(raises, 1);
	zassert_true(codec_second);
	/* Synchronous barrier returns only after any committed write. */
	zassert_ok(audio_revoke_raises(1));
	codec_due = 0;
	run(NULL);
	zassert_equal(raises, 1);
	zassert_false(codec_running);
}
ZTEST(audio, test_shutdown_reserved_when_codec_fifo_full)
{
	codec_count = CODEC_DEPTH;
	callback(STOCK_PEER_CODEC_SHUTDOWN);
	zassert_equal(last_response.status, 0);
	run(NULL);
	zassert_equal(pins[0], 16);
	zassert_false(values[0]);
	codec_due = 0;
	run(NULL);
	zassert_equal(pins[1], 12);
	zassert_false(values[1]);
}
ZTEST(audio, test_profile_variants)
{
	const uint8_t flags[] = {0, 1, 2, 8};
	for (unsigned i = 0; i < ARRAY_SIZE(flags); ++i) {
		peer.readiness = 2;
		peer.run_flags = flags[i];
		codec_running = true;
		codec_operation = STOCK_PEER_CODEC_INIT;
		codec_second = false;
		request_outstanding = false;
		operation_due = k_uptime_get() + 5000;
		run(NULL);
		zassert_equal(last_request.operation, STOCK_PEER_DSP_PROFILE);
		zassert_equal(last_request.parameters.profile,
			      (flags[i] & 9) ? STOCK_PEER_PROFILE_RUN_OVERRIDE
					     : STOCK_PEER_PROFILE_DEFAULT);
	}
}
ZTEST(audio, test_mode4_completed_skips_raw)
{
	peer.readiness = 2;
	run(NULL);
	zassert_equal(requests, 0);
	zassert_false(raw_sent);
}
ZTEST(audio, test_canceled_callback_retries_and_stale_result_ignored)
{
	mode_pending = true;
	mode_callback = (struct stock_peer_callback){
		.session = 1, .token = 55, .kind = STOCK_PEER_POWER_MODE, .mode = 4};
	state.raises_revoked = true;
	state.callbacks_settled = false;
	response_error = -EBUSY;
	run(NULL);
	zassert_equal(last_response.token, 55);
	zassert_equal(last_response.status, -ECANCELED);
	zassert_false(state.callbacks_settled);
	zassert_equal(responses, 0);
	response_error = 0;
	run(NULL);
	zassert_equal(responses, 1);
	zassert_true(state.callbacks_settled);
	outstanding = (struct stock_peer_token){1, 9, STOCK_PEER_OWNER_AUDIO};
	request_outstanding = true;
	outstanding_operation = STOCK_PEER_RAW_ACTIVE;
	peer_results[0] = (struct stock_peer_result){.token = {1, 8, STOCK_PEER_OWNER_AUDIO},
						     .operation = STOCK_PEER_RAW_ACTIVE};
	result_count = 1;
	run(NULL);
	zassert_true(request_outstanding);
	zassert_false(raw_done);
}
ZTEST(audio, test_dsp_waits_for_codec_ack_delivery)
{
	peer.readiness = 2;
	response_error = -EBUSY;
	callback(STOCK_PEER_CODEC_INIT);
	run(NULL);
	zassert_equal(requests, 0);
	zassert_equal(codec_count, 0);
	response_error = 0;
	run(NULL);
	run(NULL);
	zassert_equal(requests, 1);
	zassert_equal(last_request.operation, STOCK_PEER_DSP_PROFILE);
}
ZTEST(audio, test_forced_hold_discards_only_stale_response)
{
	mode_pending = true;
	mode_callback = (struct stock_peer_callback){.session = 1, .token = 77, .mode = 4};
	state.raises_revoked = true;
	state.callbacks_settled = false;
	response_error = -ENOENT;
	run(NULL);
	zassert_true(response_pending);
	zassert_false(state.callbacks_settled);
	peer.phase = STOCK_PEER_HELD;
	run(NULL);
	zassert_false(response_pending);
	zassert_true(state.callbacks_settled);
}
ZTEST(audio, test_no_raw_before_local_mode3)
{
	peer.readiness = 2;
	state.mode3_ready = false;
	state.mode4_ready = false;
	run(NULL);
	zassert_equal(requests, 0);
	state.mode3_ready = true;
	run(NULL);
	zassert_equal(requests, 1);
	zassert_equal(last_request.operation, STOCK_PEER_RAW_ACTIVE);
}
ZTEST(audio, test_real_workqueue_zbus_prepare_mode_and_cancel)
{
	state = (struct audio_state){.raises_revoked = true, .callbacks_settled = true};
	struct stock_peer_state ready = {
		.session = 1, .phase = STOCK_PEER_PREPARED, .board_type = 1};
	real_queue = true;
	zassert_ok(zbus_chan_pub(&stock_peer_states, &ready, K_NO_WAIT));
	zassert_ok(zbus_chan_claim(&audio_states, K_NO_WAIT));
	struct audio_command prepare = {.session = 1, .id = 1, .operation = AUDIO_PREPARE};
	zassert_ok(audio_request(&prepare));
	for (unsigned i = 0; i < 100 && !audio_snapshot().controls_ready; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_true(audio_snapshot().controls_ready);
	zassert_ok(zbus_chan_finish(&audio_states));
	struct audio_state published = {0};
	for (unsigned i = 0; i < 100 && !published.controls_ready; ++i) {
		k_sleep(K_MSEC(1));
		zassert_ok(zbus_chan_read(&audio_states, &published, K_NO_WAIT));
	}
	zassert_true(published.controls_ready);
	struct stock_peer_callback mode = {
		.session = 1, .token = 100, .kind = STOCK_PEER_POWER_MODE, .mode = 3};
	int64_t started = k_uptime_get();
	zassert_ok(zbus_chan_pub(&stock_peer_callbacks, &mode, K_NO_WAIT));
	k_sleep(K_MSEC(100));
	zassert_false(audio_snapshot().mode3_ready);
	for (unsigned i = 0; i < 400 && !audio_snapshot().mode3_ready; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_true(audio_snapshot().mode3_ready);
	zassert_true(k_uptime_get() - started >= 300);
	mode.token = 101;
	mode.mode = 4;
	zassert_ok(zbus_chan_pub(&stock_peer_callbacks, &mode, K_NO_WAIT));
	k_sleep(K_MSEC(20));
	zassert_ok(audio_revoke_raises(1));
	for (unsigned i = 0; i < 100 && !audio_snapshot().callbacks_settled; ++i) {
		k_sleep(K_MSEC(1));
	}
	zassert_true(audio_snapshot().callbacks_settled);
	k_sleep(K_MSEC(410));
	zassert_equal(raises, 0);
	zassert_false(audio_snapshot().mode4_ready);
	real_queue = false;
	struct k_work_sync sync;
	k_work_cancel_delayable_sync(&deadline, &sync);
	k_work_flush(&work, &sync);
}
ZTEST(audio, test_command_completion_capacity_reserved)
{
	state = (struct audio_state){0};
	peer.phase = STOCK_PEER_PREPARED;
	struct audio_command command = {.session = 1, .id = 9, .operation = AUDIO_PREPARE};
	zassert_ok(zbus_chan_claim(&audio_results, K_NO_WAIT));
	zassert_ok(audio_request(&command));
	zassert_equal(audio_request(&command), -EBUSY);
	run(NULL);
	zassert_true(completion_pending);
	zassert_equal(audio_request(&command), -EBUSY);
	zassert_ok(zbus_chan_finish(&audio_results));
	run(NULL);
	zassert_false(completion_pending);
}
ZTEST(audio, test_readiness_requires_raw_ack_only_if_sent)
{
	state.codec_initialized = true;
	raw_sent = true;
	raw_done = false;
	run(NULL);
	zassert_false(audio_snapshot().ready);
	raw_done = true;
	run(NULL);
	zassert_true(audio_snapshot().ready);
	raw_sent = false;
	raw_done = false;
	run(NULL);
	zassert_true(audio_snapshot().ready);
	zassert_ok(audio_revoke_raises(1));
	zassert_false(audio_snapshot().ready);
}
ZTEST(audio, test_unprepared_stop_adopts_session_without_pin_writes)
{
	state = (struct audio_state){.raises_revoked = true};
	zassert_ok(audio_revoke_raises(1));
	run(NULL);
	peer.phase = STOCK_PEER_HELD;
	struct audio_command command = {.session = 1, .id = 1, .operation = AUDIO_QUIESCE};
	zassert_ok(audio_request(&command));
	run(NULL);
	zassert_true(audio_snapshot().quiescent);
	zassert_equal(writes, 0);
	peer.phase = STOCK_PEER_PREPARED;
	command.id = 2;
	command.operation = AUDIO_PREPARE;
	zassert_ok(audio_request(&command));
	run(NULL);
	zassert_false(audio_snapshot().controls_ready);
}
ZTEST(audio, test_callback_retry_after_late_observer_error_is_deduplicated)
{
	zassert_ok(zbus_obs_set_enable(&late_subscriber, true));
	/* Fill the diagnostic channel-reference queue before delivering callback. */
	struct stock_peer_callback filler = {
		.session = 99, .token = 1, .kind = STOCK_PEER_CODEC_INIT};
	zassert_ok(zbus_chan_pub(&stock_peer_callbacks, &filler, K_NO_WAIT));
	struct stock_peer_callback callback = {
		.session = 1, .token = 200, .kind = STOCK_PEER_CODEC_INIT};
	zassert_not_equal(zbus_chan_pub(&stock_peer_callbacks, &callback, K_NO_WAIT), 0);
	zassert_not_equal(zbus_chan_pub(&stock_peer_callbacks, &callback, K_NO_WAIT), 0);
	zassert_equal(callback_count, 1);
	peer.readiness = 2;
	run(NULL);
	run(NULL);
	zassert_equal(responses, 1);
	zassert_equal(requests, 1);
	zassert_equal(codec_count, 0);
	zassert_ok(zbus_obs_set_enable(&late_subscriber, false));
}
ZTEST(audio, test_quiesce_finishes_committed_shutdown_sequence)
{
	callback(STOCK_PEER_CODEC_SHUTDOWN);
	run(NULL);
	zassert_true(codec_running);
	zassert_equal(pins[0], 16);
	peer.phase = STOCK_PEER_HELD;
	struct audio_command command = {.session = 1, .id = 1, .operation = AUDIO_QUIESCE};
	zassert_ok(audio_request(&command));
	run(NULL);
	zassert_false(state.quiescent);
	zassert_equal(command_count, 1);
	codec_due = 0;
	run(NULL);
	run(NULL);
	zassert_equal(pins[1], 12);
	zassert_true(state.quiescent);
}
ZTEST_SUITE(audio, NULL, NULL, before, NULL, NULL);
