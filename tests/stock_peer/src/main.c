/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/board_id.h>
#include <string.h>
#define STOCK_PEER_TEST
#define ZGMICRO_WS9652A_COUNTER_H
static bool ws9652a_counter_available(void)
{
	return true;
}
static bool peer_device_ready = true;
static bool mock_device_is_ready(const struct device *dev)
{
	ARG_UNUSED(dev);
	return peer_device_ready;
}
static int mock_device_init(const struct device *dev);
static int mock_board_id_get(const struct device *dev, uint32_t *id);
#define device_is_ready mock_device_is_ready
#define device_init mock_device_init
#define board_id_get mock_board_id_get
#include "../../../subsys/stock_peer/stock_peer.c"
#undef device_is_ready
#undef device_init
#undef board_id_get
static unsigned sends, releases, holds;
enum preparation_step {
	PREPARE_POWER,
	PREPARE_ARENA,
	PREPARE_PINS,
	PREPARE_STRAPS
};
static enum preparation_step preparation_steps[4];
static unsigned preparation_count;
static bool power_ready, arena_ready;
static int identity_init_result, identity_get_result;
static uint32_t detected_type;
static int pins_prepare_result;
static enum ipc_result release_result;
static uint8_t sent[64];
static bool ready(void *ctx)
{
	ARG_UNUSED(ctx);
	return true;
}
static enum ipc_result again(void *ctx)
{
	ARG_UNUSED(ctx);
	return IPC_AGAIN;
}
static enum ipc_result receive(void *ctx, struct ipc_message *msg)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(msg);
	return IPC_AGAIN;
}
static enum ipc_result send(void *ctx, uint16_t service, const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(service);
	++sends;
	memcpy(sent, data, len);
	return IPC_OK;
}
static enum ipc_result release(void *ctx, struct ipc_message *msg)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(msg);
	++releases;
	return release_result;
}
static enum ipc_result notify(void *ctx, uint32_t bit)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(bit);
	return IPC_OK;
}
const struct ipc_transport_ops go4_transport_ops = {.ready = ready,
						    .attach = again,
						    .receive = receive,
						    .send = send,
						    .release = release,
						    .notify = notify};
bool ws9652a_peer_hold(const struct device *dev)
{
	ARG_UNUSED(dev);
	++holds;
	return true;
}
uint32_t ws9652a_peer_bt_control(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}
bool ws9652a_peer_bt_enabled(uint32_t c)
{
	ARG_UNUSED(c);
	return false;
}
bool ws9652a_peer_mapping_valid(const struct device *d)
{
	ARG_UNUSED(d);
	return true;
}
bool ws9652a_peer_prepare_power(const struct device *d)
{
	ARG_UNUSED(d);
	preparation_steps[preparation_count++] = PREPARE_POWER;
	return power_ready;
}
int ws9652a_peer_prepare_pins(const struct device *d)
{
	ARG_UNUSED(d);
	preparation_steps[preparation_count++] = PREPARE_PINS;
	return pins_prepare_result;
}
void ws9652a_peer_bt_release(const struct device *d, uint32_t c)
{
	ARG_UNUSED(d);
	ARG_UNUSED(c);
}
bool ipc_arena_init(void)
{
	preparation_steps[preparation_count++] = PREPARE_ARENA;
	return arena_ready;
}
uint8_t ipc_arena_bt_readiness(void)
{
	return 1;
}
uint8_t ipc_arena_run_flags(void)
{
	return 0;
}
struct peer_media_state ipc_arena_media_state(void)
{
	return (struct peer_media_state){0};
}
static int mock_device_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	preparation_steps[preparation_count++] = PREPARE_STRAPS;
	return identity_init_result;
}
static int mock_board_id_get(const struct device *dev, uint32_t *id)
{
	ARG_UNUSED(dev);
	if (!identity_get_result) {
		*id = detected_type;
	}
	return identity_get_result;
}
static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(lanes, 0, sizeof(lanes));
	memset(ingress, 0, sizeof(ingress));
	memset(&response_ingress, 0, sizeof(response_ingress));
	memset(&inbound, 0, sizeof(inbound));
	inbound.completed_lane = -1;
	memset(&outbound, 0, sizeof(outbound));
	outbound.lane = -1;
	state = (struct stock_peer_state){.session = 1, .phase = STOCK_PEER_RELEASED};
	initialized = true;
	attached = true;
	raw_lane = -1;
	raw_attempted = raw_sent = false;
	response_reserved = false;
	sends = releases = holds = 0;
	release_result = IPC_OK;
	attempted = false;
	peer_device_ready = power_ready = arena_ready = true;
	identity_init_result = identity_get_result = 0;
	detected_type = 1;
	pins_prepare_result = 0;
	preparation_count = 0;
}
ZTEST(stock_peer, test_prepare_keeps_pins_between_arena_and_straps)
{
	const enum preparation_step expected[] = {
		PREPARE_POWER,
		PREPARE_ARENA,
		PREPARE_PINS,
		PREPARE_STRAPS,
	};
	zassert_ok(prepare());
	zassert_equal(preparation_count, ARRAY_SIZE(expected));
	zassert_mem_equal(preparation_steps, expected, sizeof(expected));
	zassert_equal(state.phase, STOCK_PEER_PREPARED);
	zassert_equal(state.board_type, 1);
}
ZTEST(stock_peer, test_prepare_stops_before_pins_on_power_or_arena_failure)
{
	power_ready = false;
	zassert_equal(prepare(), -EIO);
	zassert_equal(preparation_count, 1);

	attempted = false;
	preparation_count = 0;
	power_ready = true;
	arena_ready = false;
	zassert_equal(prepare(), -EIO);
	zassert_equal(preparation_count, 2);
}
ZTEST(stock_peer, test_prepare_propagates_pinctrl_error_before_straps)
{
	pins_prepare_result = -ENODEV;
	zassert_equal(prepare(), -ENODEV);
	zassert_equal(preparation_count, 3);
	zassert_equal(preparation_steps[2], PREPARE_PINS);
	zassert_not_equal(state.phase, STOCK_PEER_PREPARED);
}
ZTEST(stock_peer, test_prepare_propagates_board_id_sampling_error)
{
	identity_init_result = -EAGAIN;
	zassert_equal(prepare(), -EAGAIN);
	zassert_equal(preparation_count, 4);
	zassert_not_equal(state.phase, STOCK_PEER_PREPARED);
}
ZTEST(stock_peer, test_prepare_accepts_cached_board_id)
{
	identity_init_result = -EALREADY;
	zassert_ok(prepare());
	zassert_equal(state.board_type, 1);
}
ZTEST(stock_peer, test_prepare_rejects_failed_cached_board_id)
{
	identity_init_result = -EALREADY;
	identity_get_result = -ENODEV;
	zassert_equal(prepare(), -ENODEV);
	zassert_equal(state.board_type, 0);
	zassert_not_equal(state.phase, STOCK_PEER_PREPARED);
}
ZTEST(stock_peer, test_prepare_rejects_unsupported_board_id)
{
	detected_type = 2;
	zassert_equal(prepare(), -ENOTSUP);
	zassert_not_equal(state.phase, STOCK_PEER_PREPARED);
}
ZTEST(stock_peer, test_reserved_control_and_stop_barrier)
{
	struct stock_peer_request bt = {.token = {1, 1, STOCK_PEER_OWNER_BLUETOOTH},
					.operation = STOCK_PEER_PLAY};
	zassert_ok(stock_peer_request(&bt));
	zassert_equal(stock_peer_request(&bt), -EBUSY);
	struct stock_peer_request audio = {.token = {1, 1, STOCK_PEER_OWNER_AUDIO},
					   .operation = STOCK_PEER_RAW_ACTIVE};
	zassert_ok(stock_peer_request(&audio));
	bt.operation = STOCK_PEER_SHUTDOWN;
	zassert_ok(stock_peer_request(&bt));
	copy_ingress();
	zassert_ok(stock_peer_close_admission(1));
	zassert_true(lanes[0].done);
	zassert_true(lanes[1].done);
	zassert_true(lanes[2].queued);
	poll();
	zassert_equal(sends, 0);
	zassert_equal(lanes[0].result.status, -ECANCELED);
}
ZTEST(stock_peer, test_callback_reply_once_release_retry)
{
	uint8_t data[7] = {0x82, 0, 0, 0, 0, 0, 0};
	inbound.message = (struct ipc_message){.service = 23, .length = 7, .payload = data};
	struct ipc_wire_frame f = {.command = 2, .transaction = 17};
	encode_reply(&f, NULL, 0);
	release_result = IPC_AGAIN;
	progress_inbound();
	zassert_equal(sends, 1);
	zassert_equal(inbound.phase, 3);
	progress_inbound();
	zassert_equal(sends, 1);
	zassert_equal(releases, 2);
	release_result = IPC_OK;
	progress_inbound();
	zassert_equal(sends, 1);
	zassert_equal(inbound.phase, 0);
}
ZTEST(stock_peer, test_stale_callback_token)
{
	inbound.phase = 1;
	inbound.token = 7;
	struct stock_peer_callback_response r = {.session = 1, .token = 6};
	zassert_equal(stock_peer_callback_respond(&r), -ENOENT);
	r.token = 7;
	zassert_ok(stock_peer_callback_respond(&r));
	zassert_equal(stock_peer_callback_respond(&r), -EALREADY);
	copy_ingress();
	zassert_true(inbound.response_ready);
}
ZTEST(stock_peer, test_wire_vectors_limits)
{
	uint8_t data[57] = {4}, packet[64];
	uint16_t len;
	struct ipc_wire_frame f = {.command = 0, .transaction = 0x1234, .data = data, .length = 57};
	zassert_true(go4_wire_ops.encode(&f, packet, 64, &len));
	zassert_equal(len, 64);
	zassert_equal(packet[2], 0x34);
	zassert_equal(packet[3], 0x12);
	zassert_equal(packet[6], 4);
	f.reply = true;
	zassert_false(go4_wire_ops.encode(&f, packet, 64, &len));
	f.length = 48;
	zassert_true(go4_wire_ops.encode(&f, packet, 64, &len));
	zassert_equal(packet[0], 0x80);
	f.length = 58;
	f.reply = false;
	zassert_false(go4_wire_ops.encode(&f, packet, 64, &len));
}
ZTEST(stock_peer, test_dsp_vectors_and_route)
{
	struct stock_peer_request r = {.token = {1, 1, STOCK_PEER_OWNER_AUDIO},
				       .operation = STOCK_PEER_DSP_PROFILE};
	lanes[1].request = r;
	start_outbound(1);
	struct ipc_wire_frame f;
	zassert_true(go4_wire_ops.decode(outbound.payload, outbound.length, &f));
	static const uint8_t expected[27] = {4, 24, 0, 5, 0, 0, 0, 253, 0, 0, 0, 253, 0, 0,
					     0, 5,  0, 0, 0, 5, 0, 0,   0, 3, 0, 0,   0};
	zassert_equal(outbound.service, 10);
	zassert_equal(f.command, 0);
	zassert_equal(f.length, 27);
	zassert_mem_equal(f.data, expected, 27);
	r.parameters.profile = STOCK_PEER_PROFILE_RUN_OVERRIDE;
	lanes[1].request = r;
	start_outbound(1);
	zassert_true(go4_wire_ops.decode(outbound.payload, outbound.length, &f));
	static const uint8_t override[11] = {4, 8, 0, 6, 0, 0, 0, 0, 0, 0, 0};
	zassert_mem_equal(f.data, override, 11);
	uint8_t status[4] = {0}, packet[11];
	uint16_t len;
	struct ipc_wire_frame reply = {.command = 0,
				       .reply = true,
				       .transaction = outbound.transaction,
				       .data = status,
				       .length = 4};
	zassert_true(go4_wire_ops.encode(&reply, packet, sizeof(packet), &len));
	outbound.sent = true;
	inbound.message = (struct ipc_message){.service = 10, .length = len, .payload = packet};
	accept_packet();
	progress_inbound();
	zassert_true(lanes[1].done);
	zassert_ok(lanes[1].result.status);
}
ZTEST(stock_peer, test_raw_independent_of_rpc_and_stale_reply)
{
	lanes[0].request = (struct stock_peer_request){.token = {1, 1, STOCK_PEER_OWNER_BLUETOOTH},
						       .operation = STOCK_PEER_PLAY};
	start_outbound(0);
	outbound.sent = true;
	lanes[1].request = (struct stock_peer_request){.token = {1, 1, STOCK_PEER_OWNER_AUDIO},
						       .operation = STOCK_PEER_RAW_ACTIVE};
	lanes[1].queued = true;
	poll();
	zassert_true(raw_sent);
	zassert_equal(outbound.lane, 0);
	uint8_t ack = 1;
	inbound.message = (struct ipc_message){.service = 15, .length = 1, .payload = &ack};
	accept_packet();
	progress_inbound();
	zassert_true(lanes[1].done);
	zassert_equal(outbound.lane, 0);
	uint8_t reply[7];
	uint16_t len;
	struct ipc_wire_frame f = {
		.command = 1, .reply = true, .transaction = outbound.transaction + 1};
	go4_wire_ops.encode(&f, reply, 7, &len);
	inbound.message = (struct ipc_message){.service = 19, .length = len, .payload = reply};
	accept_packet();
	zassert_equal(outbound.lane, 0);
	zassert_false(lanes[0].done);
}
ZTEST(stock_peer, test_reserved_request_closure_before_delivery)
{
	struct stock_peer_request r = {.token = {1, 19, STOCK_PEER_OWNER_BLUETOOTH},
				       .operation = STOCK_PEER_PLAY};
	lanes[0].reserved = true;
	lanes[0].last_id = 19;
	lanes[0].request = r;
	ingress[0].admitted = true;
	ingress[0].reservation = r;
	zassert_ok(stock_peer_close_admission(1));
	zassert_true(lanes[0].done);
	zassert_ok(zbus_chan_pub(&stock_peer_requests, &r, K_NO_WAIT));
	copy_ingress();
	poll();
	zassert_false(lanes[0].queued);
	zassert_equal(sends, 0);
	zassert_equal(lanes[0].result.status, -ECANCELED);
}
ZTEST(stock_peer, test_result_waits_for_release_cleanup)
{
	lanes[0].request = (struct stock_peer_request){.token = {1, 1, STOCK_PEER_OWNER_BLUETOOTH},
						       .operation = STOCK_PEER_PLAY};
	start_outbound(0);
	outbound.sent = true;
	uint8_t packet[7];
	uint16_t len;
	struct ipc_wire_frame f = {
		.command = 1, .reply = true, .transaction = outbound.transaction};
	go4_wire_ops.encode(&f, packet, 7, &len);
	inbound.message = (struct ipc_message){.service = 19, .length = len, .payload = packet};
	accept_packet();
	zassert_false(lanes[0].done);
	release_result = IPC_AGAIN;
	progress_inbound();
	zassert_false(lanes[0].done);
	zassert_equal(outbound.lane, 0);
	progress_inbound();
	zassert_false(lanes[0].done);
	release_result = IPC_OK;
	progress_inbound();
	zassert_true(lanes[0].done);
	zassert_equal(outbound.lane, -1);
}
ZTEST(stock_peer, test_hold_before_prepare_consumes_one_shot)
{
	state = (struct stock_peer_state){0};
	attempted = false;
	struct stock_peer_request r = {.token = {1, 1, STOCK_PEER_OWNER_BLUETOOTH},
				       .operation = STOCK_PEER_HOLD};
	zassert_ok(stock_peer_request(&r));
	copy_ingress();
	dispatch_control();
	zassert_equal(state.phase, STOCK_PEER_HELD);
	zassert_equal(state.session, 1);
	zassert_true(state.admission_closed);
	zassert_equal(prepare(), -EALREADY);
}
ZTEST(stock_peer, test_failed_prepare_session_can_hold)
{
	peer_device_ready = false;
	state = (struct stock_peer_state){0};
	attempted = false;
	zassert_equal(prepare(), -ENODEV);
	zassert_equal(state.session, 1);
	lanes[3].request = (struct stock_peer_request){.token = {1, 2, STOCK_PEER_OWNER_BLUETOOTH},
						       .operation = STOCK_PEER_HOLD};
	lanes[3].queued = true;
	dispatch_control();
	zassert_equal(state.phase, STOCK_PEER_HELD);
}
ZTEST(stock_peer, test_hold_reservation_survives_stuck_shutdown)
{
	struct stock_peer_request r = {.token = {1, 1, STOCK_PEER_OWNER_BLUETOOTH},
				       .operation = STOCK_PEER_SHUTDOWN};
	zassert_ok(stock_peer_request(&r));
	copy_ingress();
	attached = false;
	dispatch_control();
	zassert_true(lanes[2].reserved);
	r.operation = STOCK_PEER_HOLD;
	zassert_ok(stock_peer_request(&r));
	copy_ingress();
	dispatch_control();
	zassert_equal(state.phase, STOCK_PEER_HELD);
	zassert_true(lanes[2].done);
	zassert_equal(lanes[2].result.status, -ECANCELED);
	zassert_true(lanes[3].done);
}
ZTEST(stock_peer, test_completion_capacity_retained_when_publication_contended)
{
	lanes[0].reserved = true;
	lanes[0].request = (struct stock_peer_request){.token = {1, 71, STOCK_PEER_OWNER_BLUETOOTH},
						       .operation = STOCK_PEER_PLAY};
	finish(0, 0);
	zassert_ok(zbus_chan_claim(&stock_peer_bt_results, K_NO_WAIT));
	publish();
	zassert_true(lanes[0].reserved);
	zassert_true(lanes[0].done);
	zassert_ok(zbus_chan_finish(&stock_peer_bt_results));
	publish();
	zassert_false(lanes[0].reserved);
	zassert_false(lanes[0].done);
}
ZTEST(stock_peer, test_normal_request_rejects_unreleased_session)
{
	struct stock_peer_request r = {.token = {1, 1, STOCK_PEER_OWNER_BLUETOOTH},
				       .operation = STOCK_PEER_PLAY};
	state.phase = STOCK_PEER_IDLE;
	zassert_equal(stock_peer_request(&r), -EHOSTDOWN);
	zassert_false(lanes[0].reserved);
	state.phase = STOCK_PEER_PREPARED;
	zassert_equal(stock_peer_request(&r), -EHOSTDOWN);
	zassert_false(lanes[0].reserved);
}
ZTEST(stock_peer, test_envelope_empty_opaque_and_malformed_vectors)
{
	uint8_t packet[64];
	uint16_t len;
	struct ipc_wire_frame f = {.command = 0x7f, .reply = true, .transaction = 0x1234}, decoded;
	const uint8_t expected[7] = {0xff, 0, 0x34, 0x12, 0, 0, 0};
	zassert_true(go4_wire_ops.encode(&f, packet, 64, &len));
	zassert_equal(len, 7);
	zassert_mem_equal(packet, expected, 7);
	packet[1] = 0xab;
	packet[6] = 0xcd;
	zassert_true(go4_wire_ops.decode(packet, 7, &decoded));
	zassert_equal(decoded.transaction, 0x1234);
	zassert_equal(decoded.length, 0);
	packet[4] = 1;
	zassert_false(go4_wire_ops.decode(packet, 7, &decoded));
	packet[4] = 0;
	zassert_false(go4_wire_ops.decode(packet, 6, &decoded));
	zassert_false(go4_wire_ops.encode(&f, packet, 6, &len));
}
ZTEST(stock_peer, test_pending_callback_blocks_rpc_not_raw)
{
	lanes[0].request = (struct stock_peer_request){.token = {1, 1, STOCK_PEER_OWNER_BLUETOOTH},
						       .operation = STOCK_PEER_PLAY};
	start_outbound(0);
	inbound.phase = 1;
	inbound.token = 7;
	poll();
	zassert_equal(sends, 0);
	zassert_false(outbound.sent);
	lanes[1].request = (struct stock_peer_request){.token = {1, 1, STOCK_PEER_OWNER_AUDIO},
						       .operation = STOCK_PEER_RAW_ACTIVE};
	lanes[1].queued = true;
	poll();
	zassert_equal(sends, 1);
	zassert_true(raw_sent);
	zassert_false(outbound.sent);
}
ZTEST(stock_peer, test_direct_publication_requires_matching_reservation)
{
	struct stock_peer_request bad = {.token = {1, 1, (enum stock_peer_owner) - 1},
					 .operation = STOCK_PEER_PLAY};
	zassert_ok(zbus_chan_pub(&stock_peer_requests, &bad, K_NO_WAIT));
	zassert_false(ingress[0].pending);
	bad.token.owner = STOCK_PEER_OWNER_BLUETOOTH;
	zassert_ok(zbus_chan_pub(&stock_peer_requests, &bad, K_NO_WAIT));
	zassert_false(ingress[0].pending);
	zassert_ok(stock_peer_request(&bad));
	bad.token.id = 999;
	zassert_ok(zbus_chan_pub(&stock_peer_requests, &bad, K_NO_WAIT));
	copy_ingress();
	zassert_equal(lanes[0].request.token.id, 1);
	inbound.phase = 1;
	inbound.token = 77;
	struct stock_peer_callback_response reply = {.session = 1, .token = 77};
	zassert_ok(zbus_chan_pub(&stock_peer_callback_responses, &reply, K_NO_WAIT));
	zassert_false(response_ingress.pending);
	zassert_ok(stock_peer_callback_respond(&reply));
	reply.token = 999;
	zassert_ok(zbus_chan_pub(&stock_peer_callback_responses, &reply, K_NO_WAIT));
	copy_ingress();
	zassert_equal(inbound.response.token, 77);
}
ZTEST(stock_peer, test_response_proves_callback_copied_after_publication_error)
{
	inbound.phase = 1;
	inbound.token = 9;
	inbound.publish_pending = true;
	struct stock_peer_callback_response reply = {.session = 1, .token = 9};
	zassert_ok(stock_peer_callback_respond(&reply));
	copy_ingress();
	zassert_false(inbound.publish_pending);
	zassert_true(inbound.response_ready);
}
ZTEST(stock_peer, test_stuck_release_holds_before_failing_owner)
{
	lanes[0].reserved = true;
	lanes[0].request = (struct stock_peer_request){.token = {1, 1, STOCK_PEER_OWNER_BLUETOOTH},
						       .operation = STOCK_PEER_PLAY};
	outbound.lane = 0;
	struct ipc_wire_frame request = {.command = 1, .transaction = 17};
	encode_reply(&request, NULL, 0);
	release_result = IPC_AGAIN;
	progress_inbound();
	zassert_equal(sends, 1);
	zassert_equal(inbound.phase, 3);
	zassert_false(lanes[0].done);
	inbound.cleanup_deadline = now_us();
	progress_inbound();
	zassert_equal(holds, 1);
	zassert_equal(state.phase, STOCK_PEER_FAILED);
	zassert_true(lanes[0].done);
	zassert_equal(lanes[0].result.status, -ETIMEDOUT);
	zassert_equal(sends, 1);
}
static uint32_t callback_copied;
static void copy_callback(const struct zbus_channel *channel)
{
	callback_copied = ((const struct stock_peer_callback *)zbus_chan_const_msg(channel))->token;
}
ZBUS_LISTENER_DEFINE(test_callback_copy, copy_callback);
ZBUS_SUBSCRIBER_DEFINE(test_late_failure, 1);
ZBUS_CHAN_ADD_OBS(stock_peer_callbacks, test_callback_copy, 1);
ZBUS_CHAN_ADD_OBS(stock_peer_callbacks, test_late_failure, 2);
ZTEST(stock_peer, test_late_subscriber_error_preserves_copied_callback)
{
	const struct zbus_channel *channel;
	while (!zbus_sub_wait(&test_late_failure, &channel, K_NO_WAIT)) {
	}
	struct stock_peer_callback previous = {
		.session = 1, .token = 8, .kind = STOCK_PEER_POWER_MODE, .mode = 3};
	zassert_ok(zbus_chan_pub(&stock_peer_callbacks, &previous, K_NO_WAIT));
	inbound.phase = 1;
	inbound.token = 9;
	inbound.callback = (struct stock_peer_callback){
		.session = 1, .token = 9, .kind = STOCK_PEER_POWER_MODE, .mode = 3};
	inbound.publish_pending = true;
	publish();
	zassert_equal(callback_copied, 9);
	zassert_true(inbound.publish_pending);
	struct stock_peer_callback_response response = {.session = 1, .token = 9};
	zassert_ok(stock_peer_callback_respond(&response));
	copy_ingress();
	zassert_false(inbound.publish_pending);
	zassert_ok(zbus_sub_wait(&test_late_failure, &channel, K_NO_WAIT));
}
ZTEST_SUITE(stock_peer, NULL, NULL, before, NULL, NULL);
