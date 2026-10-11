/* SPDX-License-Identifier: Apache-2.0 */
#include <cwbeat/stock_peer.h>
#include <zephyr/drivers/board_id.h>
#include <zephyr/drivers/peer/ws9652a.h>
#include <zephyr/soc/ws9652a/counter.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include "private/transport.h"
#include "private/backend.h"
#include "private/arena.h"
#include "private/profiles.h"

LOG_MODULE_REGISTER(cwbeat_stock_peer, CONFIG_CWBEAT_STOCK_PEER_LOG_LEVEL);

/* Two ordinary owners and a reserved Bluetooth control lane. A lane remains
 * reserved until its terminal result was synchronously delivered. */
struct lane {
	bool reserved, queued, done;
	uint32_t last_id;
	struct stock_peer_request request;
	struct stock_peer_result result;
};
static struct lane lanes[4];
static struct stock_peer_state state;
static struct go4_transport native;
static struct ipc_transport transport = {.ops = &go4_transport_ops, .context = &native};
#ifndef STOCK_PEER_TEST
static const struct device *const peer = DEVICE_DT_GET(
	DT_PHANDLE(DT_COMPAT_GET_ANY_STATUS_OKAY(jbl_go4_stock_ipc), peer_controller));
static const struct device *const identity = DEVICE_DT_GET(
	DT_PHANDLE(DT_COMPAT_GET_ANY_STATUS_OKAY(jbl_go4_stock_ipc), board_id));
#else
static const struct device *const peer;
static const struct device *const identity;
#endif
K_MUTEX_DEFINE(dispatch_lock);
K_SEM_DEFINE(wake, 0, 1);
static bool initialized, attempted, attached, state_dirty;
static uint32_t captured_control;
static int raw_lane = -1;
static bool raw_attempted, raw_sent;
static uint32_t raw_deadline;
static struct {
	int lane;
	bool sent;
	uint16_t service, transaction, next_transaction, length;
	uint8_t command, payload[64];
	uint32_t deadline;
	bool status_reply, raw;
} outbound = {.lane = -1};
static struct {
	unsigned phase;
	struct ipc_message message;
	uint8_t request[64], reply[64];
	uint16_t length;
	int completed_lane, completed_status;
	uint32_t token, next_token, cleanup_deadline;
	struct stock_peer_callback callback;
	bool publish_pending, response_ready;
	struct stock_peer_callback_response response;
} inbound;
/* phase: 0 receiving, 1 callback pending, 2 reply pending, 3 release pending */
static bool control(enum stock_peer_operation op)
{
	return op <= STOCK_PEER_SHUTDOWN;
}
static int lane_for(const struct stock_peer_request *r)
{
	return r->operation == STOCK_PEER_HOLD ? 3
	       : control(r->operation)         ? 2
					       : (int)r->token.owner;
}
static uint32_t now_us(void)
{
	return (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks());
}
static void finish(int lane, int status)
{
	if (lanes[lane].done) {
		return;
	}
	lanes[lane].queued = false;
	lanes[lane].done = true;
	lanes[lane].result = (struct stock_peer_result){lanes[lane].request.token,
							lanes[lane].request.operation, status};
	if (status && status != -ECANCELED) {
		LOG_ERR("Request failed: session=%u owner=%u id=%u operation=%u error=%d",
			lanes[lane].request.token.session, lanes[lane].request.token.owner,
			lanes[lane].request.token.id, lanes[lane].request.operation, status);
	}
	if (outbound.lane == lane) {
		outbound.lane = -1;
	}
	if (raw_lane == lane) {
		raw_lane = -1;
	}
}
/* Listener storage has its own spinlock: no waiting on the transport owner. */
static struct k_spinlock ingress_lock;
static struct stock_peer_state cached_state;
static struct {
	bool delivered, pending, rejected, admitted;
	uint32_t rejected_id;
	struct stock_peer_request request, reservation;
} ingress[4];
static struct {
	bool delivered, pending;
	struct stock_peer_callback_response response, reservation;
} response_ingress;
static bool response_reserved;
static bool valid_request(const struct stock_peer_request *r)
{
	return r && (int)r->token.owner >= 0 && r->token.owner <= STOCK_PEER_OWNER_AUDIO &&
	       r->token.id && r->token.session == 1 && (int)r->operation >= 0 &&
	       r->operation <= STOCK_PEER_LINK &&
	       (!control(r->operation) || r->token.owner == STOCK_PEER_OWNER_BLUETOOTH);
}
static void request_listener(const struct zbus_channel *channel)
{
	const struct stock_peer_request *r = zbus_chan_const_msg(channel);
	if (!valid_request(r)) {
		return;
	}
	int n = lane_for(r);
	k_spinlock_key_t key = k_spin_lock(&ingress_lock);
	const struct stock_peer_request *reserved = &ingress[n].reservation;
	bool matches =
		ingress[n].admitted && !ingress[n].delivered &&
		reserved->token.session == r->token.session && reserved->token.id == r->token.id &&
		reserved->token.owner == r->token.owner && reserved->operation == r->operation &&
		(r->operation != STOCK_PEER_DSP_PROFILE ||
		 reserved->parameters.profile == r->parameters.profile);
	if (matches) {
		ingress[n].request = *r;
		ingress[n].delivered = ingress[n].pending = true;
	}
	k_spin_unlock(&ingress_lock, key);
	if (matches) {
		k_sem_give(&wake);
	}
}
static void response_listener(const struct zbus_channel *channel)
{
	const struct stock_peer_callback_response *r = zbus_chan_const_msg(channel);
	k_spinlock_key_t key = k_spin_lock(&ingress_lock);
	const struct stock_peer_callback_response *reserved = &response_ingress.reservation;
	bool matches = response_reserved && !response_ingress.delivered &&
		       reserved->session == r->session && reserved->token == r->token &&
		       reserved->status == r->status &&
		       reserved->capabilities_ready == r->capabilities_ready;
	if (matches) {
		response_ingress.response = *r;
		response_ingress.delivered = response_ingress.pending = true;
	}
	k_spin_unlock(&ingress_lock, key);
	if (matches) {
		k_sem_give(&wake);
	}
}
static void copy_ingress(void)
{
	k_spinlock_key_t key = k_spin_lock(&ingress_lock);
	for (int n = 0; n < 4; ++n) {
		if (ingress[n].rejected) {
			if (lanes[n].request.token.id == ingress[n].rejected_id) {
				lanes[n].reserved = false;
				ingress[n].admitted = false;
			}
			ingress[n].rejected = false;
		}
		if (ingress[n].pending) {
			if (lanes[n].reserved && !lanes[n].done &&
			    lanes[n].request.token.id == ingress[n].request.token.id) {
				lanes[n].request = ingress[n].request;
				lanes[n].queued = true;
			}
			ingress[n].pending = false;
		}
	}
	if (response_ingress.pending) {
		if (inbound.phase == 1 && inbound.token == response_ingress.response.token) {
			inbound.response = response_ingress.response;
			inbound.response_ready = true;
			inbound.publish_pending = false;
		}
		response_ingress.pending = false;
	}
	k_spin_unlock(&ingress_lock, key);
}
ZBUS_LISTENER_DEFINE(stock_peer_request_listener, request_listener);
ZBUS_LISTENER_DEFINE(stock_peer_response_listener, response_listener);
ZBUS_CHAN_DEFINE(stock_peer_requests, struct stock_peer_request, NULL, NULL,
		 ZBUS_OBSERVERS(stock_peer_request_listener), ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_callback_responses, struct stock_peer_callback_response, NULL, NULL,
		 ZBUS_OBSERVERS(stock_peer_response_listener), ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_bt_results, struct stock_peer_result, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_audio_results, struct stock_peer_result, NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_states, struct stock_peer_state, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
ZBUS_CHAN_DEFINE(stock_peer_callbacks, struct stock_peer_callback, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0));
int stock_peer_request(const struct stock_peer_request *r)
{
	if (!r || (int)r->token.owner < 0 || r->token.owner > STOCK_PEER_OWNER_AUDIO ||
	    !r->token.id || r->token.session != 1 || (int)r->operation < 0 ||
	    r->operation > STOCK_PEER_LINK ||
	    (control(r->operation) && r->token.owner != STOCK_PEER_OWNER_BLUETOOTH)) {
		return -EINVAL;
	}
	if (k_mutex_lock(&dispatch_lock, K_NO_WAIT)) {
		return -EBUSY;
	}
	int n = lane_for(r), rc = 0;
	if (!initialized) {
		rc = -ENODEV;
	} else if (lanes[n].reserved) {
		rc = -EBUSY;
	} else if (r->token.id <= lanes[n].last_id) {
		rc = -ENOENT;
	} else if (!control(r->operation) && state.phase != STOCK_PEER_RELEASED) {
		rc = -EHOSTDOWN;
	} else if (state.admission_closed && !control(r->operation)) {
		rc = -ESHUTDOWN;
	} else {
		lanes[n].reserved = true;
		lanes[n].last_id = r->token.id;
		lanes[n].request = *r;
		k_spinlock_key_t key = k_spin_lock(&ingress_lock);
		ingress[n].delivered = false;
		ingress[n].admitted = true;
		ingress[n].reservation = *r;
		k_spin_unlock(&ingress_lock, key);
	}
	k_mutex_unlock(&dispatch_lock);
	if (rc) {
		return rc;
	}
	rc = zbus_chan_pub(&stock_peer_requests, r, K_NO_WAIT);
	k_spinlock_key_t key = k_spin_lock(&ingress_lock);
	bool delivered = ingress[n].delivered && ingress[n].request.token.id == r->token.id;
	if (rc && delivered) {
		rc = 0;
	}
	if (rc) {
		ingress[n].rejected = true;
		ingress[n].rejected_id = r->token.id;
	}
	k_spin_unlock(&ingress_lock, key);
	if (rc) {
		k_sem_give(&wake);
	}
	return rc;
}
int stock_peer_callback_respond(const struct stock_peer_callback_response *r)
{
	if (!r) {
		return -EINVAL;
	}
	if (k_mutex_lock(&dispatch_lock, K_NO_WAIT)) {
		return -EBUSY;
	}
	int rc = 0;
	if (r->session != state.session || !r->token || inbound.phase != 1 ||
	    r->token != inbound.token) {
		rc = -ENOENT;
	} else if (inbound.response_ready) {
		rc = -EALREADY;
	} else {
		k_spinlock_key_t key = k_spin_lock(&ingress_lock);
		bool pending = response_ingress.pending || response_reserved;
		k_spin_unlock(&ingress_lock, key);
		if (pending) {
			rc = -EALREADY;
		}
	}
	if (!rc) {
		k_spinlock_key_t key = k_spin_lock(&ingress_lock);
		response_reserved = true;
		response_ingress.delivered = false;
		response_ingress.reservation = *r;
		k_spin_unlock(&ingress_lock, key);
	}
	k_mutex_unlock(&dispatch_lock);
	if (rc) {
		return rc;
	}
	rc = zbus_chan_pub(&stock_peer_callback_responses, r, K_NO_WAIT);
	k_spinlock_key_t key = k_spin_lock(&ingress_lock);
	bool delivered = response_ingress.delivered && response_ingress.response.token == r->token;
	if (rc && delivered) {
		rc = 0;
	}
	response_reserved = false;
	k_spin_unlock(&ingress_lock, key);
	return rc;
}
int stock_peer_close_admission(uint32_t session)
{
	k_mutex_lock(&dispatch_lock, K_FOREVER);
	int rc = session != 1 ? -ENOENT : 0;
	if (!rc) {
		state.admission_closed = true;
		state_dirty = true;
		for (int i = 0; i < 2; ++i) {
			if (lanes[i].reserved && !lanes[i].done) {
				finish(i, -ECANCELED);
			}
		}
		k_sem_give(&wake);
	}
	k_mutex_unlock(&dispatch_lock);
	return rc;
}
struct stock_peer_state stock_peer_snapshot(void)
{
	k_spinlock_key_t key = k_spin_lock(&ingress_lock);
	struct stock_peer_state s = cached_state;
	k_spin_unlock(&ingress_lock, key);
	return s;
}
static void fail(int error)
{
	/* Hold first; no delayed callback completion may retain live peer ownership. */
	bool held = ws9652a_peer_hold(peer);
	LOG_ERR("Fault: session=%u phase=%u error=%d held=%u transport=%u "
		"attached=%u inbound=%u outbound=%d raw=%d",
		state.session, state.phase, error, held, ipc_transport_failure(&transport),
		attached, inbound.phase, outbound.lane, raw_lane);
	state.phase = STOCK_PEER_FAILED;
	state.error = held ? error : -EIO;
	state.admission_closed = true;
	state_dirty = true;
	attached = false;
	for (int i = 0; i < 4; ++i) {
		if (lanes[i].reserved && !lanes[i].done) {
			finish(i, state.error);
		}
	}
	memset(&inbound, 0, sizeof(inbound));
	inbound.completed_lane = -1;
}
static int prepare(void)
{
	if (attempted) {
		return -EALREADY;
	}
	attempted = true;
	state.session = 1;
	state_dirty = true;
	if (!device_is_ready(peer) || !ws9652a_counter_available()) {
		return -ENODEV;
	}
	captured_control = ws9652a_peer_bt_control(peer);
	if (ws9652a_peer_bt_enabled(captured_control) || !ws9652a_peer_mapping_valid(peer)) {
		return -EPERM;
	}
	if (!ws9652a_peer_prepare_power(peer) || !ipc_arena_init()) {
		return -EIO;
	}
	int err = ws9652a_peer_prepare_pins(peer);
	if (err) {
		return err;
	}
	/* The owner serializes deferred initialization while the peers are held.
	 * Subsequent reads use the cached identity after BT repurposes GPIO21. */
	err = device_init(identity);
	if (err && err != -EALREADY) {
		return err;
	}
	err = board_id_get(identity, &state.board_type);
	if (err) {
		return err;
	}
	if (state.board_type != 1) {
		return -ENOTSUP;
	}
#if defined(CONFIG_CHARGER_SC89619D) && !defined(STOCK_PEER_TEST) && DT_HAS_CHOSEN(cwbeat_charger)
	/* The routed bus is available only while peers are held. Probe the
	 * type-1 charger and apply its devicetree settings before BT release. */
	err = device_init(DEVICE_DT_GET(DT_CHOSEN(cwbeat_charger)));
	if (err && err != -EALREADY) {
		LOG_WRN("Charger probe failed: %d", err);
	}
#endif
	if (!ipc_transport_ready(&transport)) {
		return -EIO;
	}
	state.session = 1;
	state.phase = STOCK_PEER_PREPARED;
	state_dirty = true;
	LOG_INF("Prepared: session=%u board=%u", state.session, state.board_type);
	return 0;
}
static void dispatch_control(void)
{
	int n = lanes[3].queued ? 3 : 2;
	if (!lanes[n].queued) {
		return;
	}
	enum stock_peer_operation op = lanes[n].request.operation;
	int rc = 0;
	if (op == STOCK_PEER_PREPARE) {
		rc = prepare();
	} else if (state.session != 1 && op != STOCK_PEER_HOLD) {
		rc = -ENODEV;
	} else if (op == STOCK_PEER_RELEASE) {
		if (state.phase != STOCK_PEER_PREPARED || state.admission_closed) {
			rc = -EPERM;
		} else {
			__asm volatile("dmb" ::: "memory");
			ws9652a_peer_bt_release(peer, captured_control);
			state.phase = STOCK_PEER_RELEASED;
			state_dirty = true;
			LOG_INF("Released: session=%u", state.session);
		}
	} else if (op == STOCK_PEER_HOLD) {
		state.session = 1;
		attempted = true;
		if (lanes[2].reserved && !lanes[2].done) {
			finish(2, -ECANCELED);
		}
		state.admission_closed = true;
		if (!ws9652a_peer_hold(peer)) {
			rc = -EIO;
		}
		state.phase = rc ? STOCK_PEER_FAILED : STOCK_PEER_HELD;
		state.error = rc;
		state_dirty = true;
		attached = false;
		for (int i = 0; i < 2; ++i) {
			if (lanes[i].reserved && !lanes[i].done) {
				finish(i, -ECANCELED);
			}
		}
		memset(&inbound, 0, sizeof(inbound));
		inbound.completed_lane = -1;
		if (!rc) {
			LOG_INF("Held: session=%u", state.session);
		}
	} else if (state.phase != STOCK_PEER_RELEASED) {
		rc = -EPERM;
	} else if (!attached) {
		return;
	} else {
		enum ipc_result result =
			ipc_transport_notify(&transport, op == STOCK_PEER_WAKE ? 7 : 8);
		if (result == IPC_AGAIN) {
			return;
		}
		rc = result == IPC_OK ? 0 : -EIO;
		if (rc) {
			LOG_ERR("Notification failed: operation=%u transport=%u", op,
				ipc_transport_failure(&transport));
		}
	}
	finish(n, rc);
	if (rc && op == STOCK_PEER_PREPARE) {
		fail(rc);
	}
}
static void start_outbound(int lane)
{
	struct stock_peer_request *r = &lanes[lane].request;
	if (state.phase != STOCK_PEER_RELEASED || state.admission_closed) {
		finish(lane, -ESHUTDOWN);
		return;
	}
	uint8_t data[57] = {0};
	unsigned length = 0, timeout = 100000;
	outbound.service = 0x13;
	outbound.command = 1;
	outbound.status_reply = false;
	outbound.raw = false;
	switch (r->operation) {
	case STOCK_PEER_RAW_ACTIVE:
		outbound.raw = true;
		outbound.service = 0x0f;
		data[0] = 4;
		length = 4;
		timeout = 2000000;
		break;
	case STOCK_PEER_DSP_PROFILE:
		if ((int)r->parameters.profile < 0 ||
		    r->parameters.profile > STOCK_PEER_PROFILE_RUN_OVERRIDE) {
			finish(lane, -EINVAL);
			return;
		}
		outbound.service = 0x0a;
		outbound.command = 0;
		timeout = 200000;
		outbound.status_reply = true;
		length = r->parameters.profile == STOCK_PEER_PROFILE_DEFAULT
				 ? sizeof(stored_profile)
				 : sizeof(override_profile);
		memcpy(data,
		       r->parameters.profile == STOCK_PEER_PROFILE_DEFAULT ? stored_profile
									   : override_profile,
		       length);
		break;
	case STOCK_PEER_FEEDBACK_DISABLE:
		outbound.command = 6;
		break;
	case STOCK_PEER_PLAY:
		break;
	case STOCK_PEER_PAUSE:
		outbound.command = 2;
		break;
	case STOCK_PEER_NEXT:
		outbound.command = 3;
		break;
	case STOCK_PEER_PREVIOUS:
		outbound.command = 4;
		break;
	case STOCK_PEER_VOLUME_UP:
	case STOCK_PEER_VOLUME_DOWN:
		outbound.service = 0x11;
		outbound.command = 2;
		data[0] = r->operation == STOCK_PEER_VOLUME_UP ? 2 : 3;
		length = 4;
		timeout = 200000;
		outbound.status_reply = true;
		break;
	case STOCK_PEER_LINK:
		outbound.service = 0x16;
		outbound.command = 1;
		outbound.status_reply = true;
		break;
	case STOCK_PEER_DISCOVER:
	case STOCK_PEER_PAIR:
		outbound.service = 0x12;
		outbound.command = r->operation == STOCK_PEER_PAIR ? 6 : 3;
		break;
	default:
		finish(lane, -EINVAL);
		return;
	}
	if (outbound.next_transaction == UINT16_MAX) {
		finish(lane, -EOVERFLOW);
		return;
	}
	outbound.transaction = outbound.next_transaction++;
	if (outbound.raw) {
		memcpy(outbound.payload, data, length);
		outbound.length = length;
	} else {
		struct ipc_wire_frame frame = {.command = outbound.command,
					       .transaction = outbound.transaction,
					       .data = data,
					       .length = length};
		if (!go4_wire_ops.encode(&frame, outbound.payload, sizeof(outbound.payload),
					 &outbound.length)) {
			finish(lane, -EINVAL);
			return;
		}
	}
	outbound.lane = lane;
	outbound.sent = false;
	outbound.deadline = now_us() + timeout;
	lanes[lane].queued = false;
}
static void encode_reply(const struct ipc_wire_frame *f, const uint8_t *data, unsigned len)
{
	struct ipc_wire_frame reply = {.command = f->command,
				       .transaction = f->transaction,
				       .reply = true,
				       .data = data,
				       .length = len};
	if (!go4_wire_ops.encode(&reply, inbound.reply, 64, &inbound.length)) {
		fail(-EBADMSG);
		return;
	}
	inbound.phase = 2;
	inbound.cleanup_deadline = now_us() + 5000000;
}
static void accept_packet(void)
{
	memcpy(inbound.request, (const void *)inbound.message.payload, inbound.message.length);
	struct ipc_wire_frame f;
	inbound.length = 0;
	inbound.completed_lane = -1;
	inbound.cleanup_deadline = now_us() + 5000000;
	if (inbound.message.service == 15) {
		if (raw_lane >= 0 && raw_sent && inbound.message.length == 1 &&
		    inbound.request[0] == 1) {
			inbound.completed_lane = raw_lane;
			inbound.completed_status = 0;
		}
		inbound.phase = 3;
		return;
	}
	if (!go4_wire_ops.decode(inbound.request, inbound.message.length, &f)) {
		inbound.phase = 3;
		return;
	}
	if (f.reply) {
		if (outbound.lane >= 0 && !outbound.raw && outbound.sent &&
		    f.command == outbound.command && f.transaction == outbound.transaction &&
		    inbound.message.service == outbound.service) {
			int rc = f.length != (outbound.status_reply ? 4 : 0) ? -EBADMSG : 0;
			if (!rc && outbound.status_reply &&
			    (f.data[0] || f.data[1] || f.data[2] || f.data[3])) {
				rc = -EIO;
			}
			inbound.completed_lane = outbound.lane;
			inbound.completed_status = rc;
		}
		inbound.phase = 3;
		return;
	}
	static const uint8_t identity[16] = {'e', 'x', 'c', 'e', 'l', 'p', 'o', 'i',
					     'n', 't', '.', 4,   255, 228, 32,  3};
	if (inbound.message.service == 8 && f.command == 29 && !f.length) {
		encode_reply(&f, identity, 16);
		return;
	}
	enum stock_peer_callback_kind kind;
	if ((inbound.message.service == 14 && f.command == 1 && !f.length) ||
	    (inbound.message.service == 23 &&
	     (f.command == 2 || f.command == 4 || f.command == 5) && f.length)) {
		static const uint8_t error[4] = {255, 255, 255, 255};
		encode_reply(&f, error, 4);
		return;
	}
	if (inbound.message.service == 14 && f.command == 1 && f.length >= 1) {
		kind = STOCK_PEER_POWER_MODE;
	} else if (inbound.message.service == 23 && !f.length &&
		   (f.command == 1 || f.command == 2 || f.command == 4 || f.command == 5)) {
		kind = f.command == 1   ? STOCK_PEER_CODEC_CAPABILITIES
		       : f.command == 2 ? STOCK_PEER_CODEC_INIT
		       : f.command == 4 ? STOCK_PEER_CODEC_SHUTDOWN
					: STOCK_PEER_CODEC_ENABLE;
	} else {
		inbound.phase = 3;
		return;
	}
	inbound.token = ++inbound.next_token;
	if (!inbound.token) {
		fail(-EOVERFLOW);
		return;
	}
	inbound.callback = (struct stock_peer_callback){
		state.session, inbound.token, kind, kind == STOCK_PEER_POWER_MODE ? f.data[0] : 0};
	inbound.phase = 1;
	inbound.publish_pending = true;
}
static void progress_inbound(void)
{
	if (inbound.phase >= 2 && (int32_t)(now_us() - inbound.cleanup_deadline) >= 0) {
		LOG_ERR("Inbound cleanup timed out: phase=%u service=%u token=%u",
			inbound.phase, inbound.message.service, inbound.token);
		fail(-ETIMEDOUT);
		return;
	}
	enum ipc_result rc = IPC_AGAIN;
	if (inbound.phase == 1) {
		if (!inbound.response_ready) {
			return;
		}
		struct ipc_wire_frame f;
		if (!go4_wire_ops.decode(inbound.request, inbound.message.length, &f)) {
			fail(-EBADMSG);
			return;
		}
		uint8_t data[48] = {0};
		unsigned len = inbound.callback.kind == STOCK_PEER_CODEC_CAPABILITIES ? 48 : 4;
		if (len == 48) {
			if (!inbound.response.status && inbound.response.capabilities_ready) {
				data[0] = data[8] = data[12] = 1;
			}
		} else if (inbound.response.status) {
			memset(data, 255, 4);
		}
		inbound.response_ready = false;
		encode_reply(&f, data, len);
	}
	if (inbound.phase == 2) {
		rc = ipc_transport_send(&transport, inbound.message.service, inbound.reply,
					inbound.length);
		if (rc == IPC_OK) {
			inbound.phase = 3;
		}
	}
	if (inbound.phase == 3) {
		rc = ipc_transport_release(&transport, &inbound.message);
		if (rc == IPC_OK) {
			if (inbound.completed_lane >= 0) {
				finish(inbound.completed_lane, inbound.completed_status);
				inbound.completed_lane = -1;
			}
			inbound.phase = 0;
			inbound.token = 0;
			inbound.publish_pending = false;
		}
	} else if (!inbound.phase) {
		rc = ipc_transport_receive(&transport, &inbound.message);
		if (rc == IPC_OK) {
			if (inbound.message.length > 64 ||
			    (inbound.message.length && !inbound.message.payload)) {
				fail(-EBADMSG);
				return;
			}
			accept_packet();
		}
	}
	if (rc == IPC_INVALID) {
		fail(-EIO);
	}
}
static void poll(void)
{
	if (state.phase == STOCK_PEER_RELEASED) {
		if (!ws9652a_counter_available()) {
			fail(-ENODEV);
			return;
		}
		if (!attached) {
			enum ipc_result rc = ipc_transport_attach(&transport);
			if (rc == IPC_INVALID) {
				fail(-EIO);
				return;
			}
			attached = rc == IPC_OK;
			if (attached) {
				LOG_INF("IPC attached: session=%u", state.session);
			}
		}
		if (attached) {
			progress_inbound();
		}
	}
	/* Finish callback response ownership before posting shutdown event8. */
	if (lanes[3].queued ||
	    !(lanes[2].queued && lanes[2].request.operation == STOCK_PEER_SHUTDOWN &&
	      inbound.phase)) {
		dispatch_control();
	}
	if (state.phase != STOCK_PEER_RELEASED || !attached) {
		return;
	}
	for (int i = 0; i < 2; ++i) {
		if (lanes[i].queued && lanes[i].request.operation == STOCK_PEER_RAW_ACTIVE) {
			if (raw_attempted) {
				finish(i, -EALREADY);
			} else {
				raw_attempted = true;
				raw_lane = i;
				raw_sent = false;
				raw_deadline = now_us() + 2000000;
				lanes[i].queued = false;
			}
		}
	}
	if (raw_lane >= 0) {
		if (inbound.completed_lane == raw_lane &&
		    inbound.phase == 3) { /* Release before completion. */
		} else if ((int32_t)(now_us() - raw_deadline) >= 0) {
			LOG_ERR("Raw active-mode timed out: sent=%u inbound=%u", raw_sent,
				inbound.phase);
			finish(raw_lane, -ETIMEDOUT);
		} else if (!raw_sent) {
			static const uint8_t mode[4] = {4, 0, 0, 0};
			enum ipc_result rc = ipc_transport_send(&transport, 15, mode, 4);
			if (rc == IPC_OK) {
				raw_sent = true;
			} else if (rc == IPC_INVALID) {
				fail(-EIO);
			}
		}
	}
	if (outbound.lane < 0 && !inbound.phase) {
		for (int i = 0; i < 2; ++i) {
			if (lanes[i].queued) {
				start_outbound(i);
				break;
			}
		}
	}
	if (outbound.lane >= 0) {
		if (inbound.completed_lane == outbound.lane &&
		    inbound.phase == 3) { /* Reply accepted: cleanup owns completion. */
		} else if ((int32_t)(now_us() - outbound.deadline) >= 0) {
			LOG_ERR("RPC timed out: service=%u command=%u transaction=%u sent=%u "
				"inbound=%u",
				outbound.service, outbound.command, outbound.transaction,
				outbound.sent, inbound.phase);
			finish(outbound.lane, -ETIMEDOUT);
		} else if (!outbound.sent && (!inbound.phase || outbound.raw)) {
			enum ipc_result rc = ipc_transport_send(&transport, outbound.service,
								outbound.payload, outbound.length);
			if (rc == IPC_OK) {
				outbound.sent = true;
			} else if (rc == IPC_INVALID) {
				fail(-EIO);
			}
		}
	}
	struct peer_media_state media = ipc_arena_media_state();
	uint8_t ready = ipc_arena_bt_readiness(), flags = ipc_arena_run_flags();
	if (state.readiness != ready || state.run_flags != flags ||
	    state.connection != media.connection || state.playing != media.playing ||
	    state.auxiliary != media.auxiliary) {
		state.readiness = ready;
		state.run_flags = flags;
		state.connection = media.connection;
		state.playing = media.playing;
		state.auxiliary = media.auxiliary;
		state_dirty = true;
	}
}
static void publish(void)
{
	for (int i = 0; i < 4; ++i) {
		k_mutex_lock(&dispatch_lock, K_FOREVER);
		bool done = lanes[i].done;
		struct stock_peer_result r = lanes[i].result;
		k_mutex_unlock(&dispatch_lock);
		if (done && !zbus_chan_pub(r.token.owner == STOCK_PEER_OWNER_AUDIO
						   ? &stock_peer_audio_results
						   : &stock_peer_bt_results,
					   &r, K_NO_WAIT)) {
			k_mutex_lock(&dispatch_lock, K_FOREVER);
			if (lanes[i].done && lanes[i].result.token.id == r.token.id) {
				lanes[i].done = false;
				lanes[i].reserved = false;
				k_spinlock_key_t key = k_spin_lock(&ingress_lock);
				ingress[i].admitted = false;
				k_spin_unlock(&ingress_lock, key);
			}
			k_mutex_unlock(&dispatch_lock);
		}
	}
	k_mutex_lock(&dispatch_lock, K_FOREVER);
	bool dirty = state_dirty;
	struct stock_peer_state s = state;
	k_spinlock_key_t key = k_spin_lock(&ingress_lock);
	cached_state = s;
	k_spin_unlock(&ingress_lock, key);
	state_dirty = false;
	bool pending = inbound.publish_pending && inbound.phase == 1 && inbound.token;
	struct stock_peer_callback cb = inbound.callback;
	k_mutex_unlock(&dispatch_lock);
	if (dirty && zbus_chan_pub(&stock_peer_states, &s, K_NO_WAIT)) {
		k_mutex_lock(&dispatch_lock, K_FOREVER);
		state_dirty = true;
		k_mutex_unlock(&dispatch_lock);
	}
	if (pending && !zbus_chan_pub(&stock_peer_callbacks, &cb, K_NO_WAIT)) {
		k_mutex_lock(&dispatch_lock, K_FOREVER);
		if (inbound.token == cb.token) {
			inbound.publish_pending = false;
		}
		k_mutex_unlock(&dispatch_lock);
	}
}
#ifndef STOCK_PEER_TEST
static void thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (true) {
		k_mutex_lock(&dispatch_lock, K_FOREVER);
		bool retry = state.phase == STOCK_PEER_RELEASED || state_dirty ||
			     inbound.publish_pending;
		for (int i = 0; i < 4; ++i) {
			retry |= lanes[i].done;
		}
		k_mutex_unlock(&dispatch_lock);
		k_sem_take(&wake, retry ? K_USEC(100) : K_FOREVER);
		k_mutex_lock(&dispatch_lock, K_FOREVER);
		if (initialized) {
			copy_ingress();
			poll();
		}
		k_mutex_unlock(&dispatch_lock);
		publish();
	}
}
K_THREAD_DEFINE(stock_peer_thread, 1536, thread, NULL, NULL, NULL, 5, 0, 0);
BUILD_ASSERT(90 > CONFIG_ZBUS_CHANNELS_SYS_INIT_PRIORITY,
	     "Peer listeners must initialize before adapter");
static int init(void)
{
	inbound.completed_lane = -1;
	initialized = true;
	state_dirty = true;
	k_sem_give(&wake);
	return 0;
}
SYS_INIT(init, APPLICATION, 90);
#endif
