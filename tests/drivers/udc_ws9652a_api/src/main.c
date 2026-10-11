/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include "udc_common.h"

/* Keep production endpoint registration and Zephyr's real UDC common layer.
 * Only the worker scheduling and hardware boundary are replaced: each test
 * explicitly advances one mutex-protected polling iteration.
 */
static k_tid_t fake_thread_create(struct k_thread *thread, k_thread_stack_t *stack,
				 size_t size, k_thread_entry_t entry, void *p1,
				 void *p2, void *p3, int priority, uint32_t options,
				 k_timeout_t delay)
{
	ARG_UNUSED(stack); ARG_UNUSED(size); ARG_UNUSED(entry); ARG_UNUSED(p1);
	ARG_UNUSED(p2); ARG_UNUSED(p3); ARG_UNUSED(priority); ARG_UNUSED(options);
	ARG_UNUSED(delay);
	return thread;
}

static int fake_thread_name_set(k_tid_t thread, const char *name)
{
	ARG_UNUSED(thread); ARG_UNUSED(name);
	return 0;
}

#define k_thread_create fake_thread_create
#define k_thread_name_set fake_thread_name_set
#define CONFIG_UDC_WS9652A_THREAD_PRIORITY 2
#define CONFIG_UDC_WS9652A_POLL_INTERVAL_MS 1
static int64_t fake_now;
#define k_uptime_get() fake_now
#include "../../../../drivers/usb/udc_ws9652a.c"
#undef k_uptime_get
#undef k_thread_create
#undef k_thread_name_set

#define BULK_IN  0x82
#define BULK_OUT 0x02
#define MAX_EVENTS 64

struct recorded_event {
	enum udc_event_type type;
	struct net_buf *buf;
	int status;
};

struct transmitted_packet {
	uint8_t ep;
	size_t size;
	bool data_end;
	uint8_t data[WS9652A_USB_MAX_PACKET];
};

static struct {
	struct ws9652a_usb_event next;
	void *rx[3];
	size_t rx_size[3];
	uint16_t count[3];
	unsigned int arms[3], acks[3], cancels[256], disables[256], configs[256];
	bool stalled[256], ack_end[3], connected;
	uint8_t address, active_tx_mask;
	int rx_error, tx_error, config_error;
	unsigned int resets, setup_acks;
	unsigned int inits, hw_disables, connects, polls, reset_attempts, reset_rejects;
	struct transmitted_packet tx[32];
	size_t tx_count;
	struct {
		bool inject, fifo_waiting;
		uint8_t packet[WS9652A_USB_MAX_PACKET];
		size_t size;
		unsigned int arms_before, stranded;
	} bulk_at_ack;
} hw;
static struct recorded_event events[MAX_EVENTS];
static size_t event_count;
static struct net_buf *buffers[16];
static size_t buffer_count;
static struct ws9652a_udc_data priv;
static struct udc_data common;
static const struct ws9652a_udc_config config;
static const struct device dev = {
	.name = "test_ws9652a", .config = &config, .data = &common, .api = &ws9652a_api,
};

void ws9652a_usb_hw_init(const struct ws9652a_usb_hw *unused)
{
	ARG_UNUSED(unused);
	hw.inits++;
	memset(&hw.next, 0, sizeof(hw.next));
}

void ws9652a_usb_hw_reset(const struct ws9652a_usb_hw *unused)
{
	ARG_UNUSED(unused);
	hw.resets++;
	memset(hw.rx, 0, sizeof(hw.rx));
	hw.address = 0;
}

void ws9652a_usb_hw_disable(const struct ws9652a_usb_hw *unused)
{
	ARG_UNUSED(unused);
	hw.hw_disables++;
	hw.connected = false;
	memset(hw.rx, 0, sizeof(hw.rx));
}

void ws9652a_usb_hw_connect(const struct ws9652a_usb_hw *unused, bool connected)
{
	ARG_UNUSED(unused);
	hw.connected = connected;
	if (connected) {
		zassert_not_null(hw.rx[0], "Arm EP0 before connecting");
		hw.connects++;
	}
}

void ws9652a_usb_hw_poll(const struct ws9652a_usb_hw *unused,
		       struct ws9652a_usb_event *event, uint8_t active_tx_mask)
{
	ARG_UNUSED(unused);
	zassert_equal(active_tx_mask & BIT(0), 0, "Control completion remains interrupt-driven");
	hw.polls++;
	hw.active_tx_mask = active_tx_mask;
	*event = hw.next;
	memset(&hw.next, 0, sizeof(hw.next));
}

void ws9652a_usb_hw_set_address(const struct ws9652a_usb_hw *unused, uint8_t address)
{
	ARG_UNUSED(unused);
	hw.address = address;
}

int ws9652a_usb_hw_ep_configure(const struct ws9652a_usb_hw *unused, uint8_t ep,
			      uint16_t mps)
{
	ARG_UNUSED(unused);
	zassert_equal(mps, 64);
	hw.configs[ep]++;
	return hw.config_error;
}

void ws9652a_usb_hw_ep_cancel(const struct ws9652a_usb_hw *unused, uint8_t ep)
{
	ARG_UNUSED(unused);
	hw.cancels[ep]++;
	if (USB_EP_DIR_IS_OUT(ep)) {
		hw.rx[ep] = NULL;
	}
}

void ws9652a_usb_hw_ep_disable(const struct ws9652a_usb_hw *unused, uint8_t ep)
{
	ARG_UNUSED(unused);
	hw.disables[ep]++;
	if (USB_EP_DIR_IS_OUT(ep)) {
		hw.rx[ep] = NULL;
	}
}

int ws9652a_usb_hw_rx_arm(const struct ws9652a_usb_hw *unused, uint8_t ep,
			void *buffer, size_t size)
{
	ARG_UNUSED(unused);
	zassert_true(ep < 3);
	zassert_equal((uintptr_t)buffer % 4, 0);
	if (hw.rx_error != 0) {
		return hw.rx_error;
	}
	hw.arms[ep]++;
	hw.rx[ep] = buffer;
	hw.rx_size[ep] = size;
	if (ep == USB_EP_GET_IDX(BULK_OUT) && hw.bulk_at_ack.fifo_waiting) {
		/* The next packet waited in the FIFO after the preceding ACK. */
		zassert_true(hw.bulk_at_ack.size <= size);
		memcpy(buffer, hw.bulk_at_ack.packet, hw.bulk_at_ack.size);
		hw.count[ep] = hw.bulk_at_ack.size;
		hw.next.rx |= BIT(ep);
		hw.bulk_at_ack.fifo_waiting = false;
	}
	return 0;
}

uint16_t ws9652a_usb_hw_rx_count(const struct ws9652a_usb_hw *unused, uint8_t ep)
{
	ARG_UNUSED(unused);
	return hw.count[ep];
}

void ws9652a_usb_hw_rx_ack(const struct ws9652a_usb_hw *unused, uint8_t ep,
			 bool data_end)
{
	ARG_UNUSED(unused);
	if (ep == 0) {
		zassert_not_null(hw.rx[ep], "EP0 must arm storage before releasing RXREADY");
	}
	hw.acks[ep]++;
	hw.ack_end[ep] = data_end;
	if (ep == USB_EP_GET_IDX(BULK_OUT) && hw.bulk_at_ack.inject) {
		hw.bulk_at_ack.inject = false;
		if (hw.arms[ep] != hw.bulk_at_ack.arms_before) {
			/* Captured hardware failure: rearming while the old packet is
			 * held coalesces RXCOUNT, then ACK strands the trailing packet.
			 */
			hw.count[ep] += hw.bulk_at_ack.size;
			hw.bulk_at_ack.stranded++;
		} else {
			hw.bulk_at_ack.fifo_waiting = true;
		}
	}
}

int ws9652a_usb_hw_tx_start(const struct ws9652a_usb_hw *unused, uint8_t ep,
			  const void *buffer, size_t size, bool data_end)
{
	ARG_UNUSED(unused);
	zassert_equal((uintptr_t)buffer % 4, 0);
	zassert_true(size <= 64);
	if (hw.tx_error != 0) {
		return hw.tx_error;
	}
	zassert_true(hw.tx_count < ARRAY_SIZE(hw.tx));
	struct transmitted_packet *packet = &hw.tx[hw.tx_count++];

	packet->ep = ep;
	packet->size = size;
	packet->data_end = data_end;
	memcpy(packet->data, buffer, size);
	return 0;
}

void ws9652a_usb_hw_stall(const struct ws9652a_usb_hw *unused, uint8_t ep, bool stalled)
{
	ARG_UNUSED(unused);
	hw.stalled[ep] = stalled;
}

void ws9652a_usb_hw_setup_end_ack(const struct ws9652a_usb_hw *unused)
{
	ARG_UNUSED(unused);
	hw.setup_acks++;
}

static int event_cb(const struct device *device, const struct udc_event *event)
{
	zassert_equal_ptr(device, &dev);
	if (event->type == UDC_EVT_RESET) {
		hw.reset_attempts++;
		if (hw.reset_rejects != 0) {
			hw.reset_rejects--;
			return -ENOMSG;
		}
	}
	zassert_true(event_count < MAX_EVENTS);
	struct recorded_event *record = &events[event_count++];

	record->type = event->type;
	if (event->type == UDC_EVT_EP_REQUEST) {
		record->buf = event->buf;
		record->status = udc_get_buf_info(event->buf)->err;
	} else {
		record->status = event->status;
	}
	return 0;
}

static void tick(void)
{
	udc_lock_internal(&dev, K_FOREVER);
	ws9652a_poll(&dev);
	udc_unlock_internal(&dev);
}

static void reset_bus(void)
{
	hw.next.bus = WS9652A_USB_BUS_RESET;
	tick();
}

static struct net_buf *track(struct net_buf *buf)
{
	zassert_not_null(buf);
	zassert_true(buffer_count < ARRAY_SIZE(buffers));
	buffers[buffer_count++] = buf;
	return buf;
}

static void enqueue(struct net_buf *buf)
{
	zassert_ok(udc_ep_enqueue(&dev, buf));
}

static struct net_buf *request(uint8_t ep, size_t size, bool input)
{
	struct net_buf *buf = track(udc_ep_buf_alloc(&dev, ep, size));

	if (input) {
		for (size_t i = 0; i < size; i++) {
			net_buf_add_u8(buf, i);
		}
	}
	return buf;
}

static unsigned int completions(struct net_buf *buf, int status)
{
	unsigned int count = 0;

	for (size_t i = 0; i < event_count; i++) {
		if (events[i].type == UDC_EVT_EP_REQUEST && events[i].buf == buf) {
			zassert_equal(events[i].status, status);
			count++;
		}
	}
	return count;
}

static void receive(uint8_t ep, const void *data, size_t size)
{
	zassert_not_null(hw.rx[ep], "receiving without DMA storage");
	zassert_true(size <= hw.rx_size[ep]);
	memcpy(hw.rx[ep], data, size);
	hw.count[ep] = size;
	if (ep == 0) {
		hw.next.ep0_csr |= WS9652A_USB_EP0_RX_READY;
	} else {
		hw.next.rx |= BIT(ep);
	}
}

static void tx_ack(uint8_t ep)
{
	hw.next.tx |= BIT(ep);
	tick();
}

static void receive_bulk_at_ack(const void *data, size_t size)
{
	zassert_true(size <= sizeof(hw.bulk_at_ack.packet));
	hw.bulk_at_ack.inject = true;
	hw.bulk_at_ack.size = size;
	hw.bulk_at_ack.arms_before = hw.arms[USB_EP_GET_IDX(BULK_OUT)];
	memcpy(hw.bulk_at_ack.packet, data, size);
}

static void setup_packet(bool input, uint16_t length)
{
	uint8_t bytes[8] = {input ? 0x80 : 0, 1, 0, 0, 0, 0};

	sys_put_le16(length, bytes + 6);
	receive(0, bytes, sizeof(bytes));
}

static struct net_buf *setup_request(bool input, uint16_t length)
{
	struct net_buf *buf = track(udc_ctrl_setup_alloc(&dev));

	enqueue(buf);
	setup_packet(input, length);
	tick();
	zassert_equal(completions(buf, 0), 1);
	zassert_equal(buf->len, 8);
	return buf;
}

static void enable_bulk(void)
{
	zassert_ok(udc_ep_enable(&dev, BULK_IN, USB_EP_TYPE_BULK, 64, 0));
	zassert_ok(udc_ep_enable(&dev, BULK_OUT, USB_EP_TYPE_BULK, 64, 0));
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(&hw, 0, sizeof(hw));
	memset(&priv, 0, sizeof(priv));
	memset(&common, 0, sizeof(common));
	memset(events, 0, sizeof(events));
	common.priv = &priv;
	event_count = 0;
	buffer_count = 0;
	fake_now = 1000;
	zassert_ok(ws9652a_preinit(&dev));
	zassert_ok(udc_init(&dev, event_cb, &priv));
	zassert_ok(udc_enable(&dev));
	reset_bus();
}

static void after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (udc_is_enabled(&dev)) {
		zassert_ok(udc_disable(&dev));
	}
	zassert_ok(udc_shutdown(&dev));
	for (size_t i = 0; i < buffer_count; i++) {
		net_buf_unref(buffers[i]);
	}
}

ZTEST(udc_ws9652a_api, test_caps_and_attach_wait_for_reset)
{
	zassert_true(common.caps.out_ack);
	zassert_false(common.caps.addr_before_status);
	zassert_equal(udc_ep_enable(&dev, 1, USB_EP_TYPE_BULK, 64, 0), -ENODEV);
	zassert_equal(udc_ep_enable(&dev, 0x81, USB_EP_TYPE_BULK, 64, 0), -ENODEV);
	zassert_ok(udc_disable(&dev));
	zassert_ok(udc_enable(&dev));
	setup_packet(true, 8);
	tick();
	zassert_false(common.setup_pending);
	zassert_true(priv.awaiting_reset);
	reset_bus();
	zassert_false(priv.awaiting_reset);
	zassert_true(hw.connected);
}

ZTEST(udc_ws9652a_api, test_setup_cached_until_stack_queues_request)
{
	setup_packet(true, 18);
	tick();
	zassert_true(common.setup_pending);
	zassert_equal(priv.phase, WS9652A_DATA_IN);
	struct net_buf *late = request(0x80, 1, true);

	enqueue(late);
	zassert_equal(completions(late, -ECONNRESET), 1);
	struct net_buf *setup = track(udc_ctrl_setup_alloc(&dev));

	enqueue(setup);
	zassert_false(common.setup_pending);
	zassert_equal(completions(setup, 0), 1);
	zassert_equal(sys_get_le16(setup->data + 6), 18);
}

ZTEST(udc_ws9652a_api, test_control_in_multipacket_zlp_and_auto_status_out)
{
	setup_request(true, 255);
	struct net_buf *buf = request(0x80, 128, true);

	udc_get_buf_info(buf)->zlp = true;
	enqueue(buf);
	tick();
	zassert_equal(hw.tx_count, 1);
	zassert_equal(hw.tx[0].size, 64);
	zassert_false(hw.tx[0].data_end);
	zassert_equal(completions(buf, 0), 0);
	tx_ack(0);
	zassert_equal(hw.tx_count, 2);
	zassert_equal(hw.tx[1].data[0], 64);
	zassert_false(hw.tx[1].data_end);
	tx_ack(0);
	zassert_equal(hw.tx_count, 3);
	zassert_equal(hw.tx[2].size, 0);
	zassert_true(hw.tx[2].data_end);
	zassert_equal(completions(buf, 0), 0);
	tx_ack(0);
	zassert_equal(completions(buf, 0), 1);
	zassert_equal(priv.phase, WS9652A_SETUP);
	size_t old_events = event_count;

	receive(0, "", 0);
	tick();
	zassert_true(hw.ack_end[0]);
	zassert_equal(event_count, old_events);
}

ZTEST(udc_ws9652a_api, test_control_out_waits_for_data_buffer_and_next_setup_coalesces)
{
	setup_request(false, 7);
	unsigned int acks = hw.acks[0];

	receive(0, "payload", 7);
	tick();
	zassert_equal(hw.acks[0], acks);
	zassert_true(priv.ep0_pending);
	struct net_buf *out = track(udc_ctrl_data_alloc(&dev, 0, 7));

	enqueue(out);
	tick();
	zassert_equal(completions(out, 0), 1);
	zassert_mem_equal(out->data, "payload", 7);
	zassert_equal(priv.phase, WS9652A_STATUS_IN);
	zassert_equal(hw.acks[0], acks, "Hold final OUT until the stack accepts it");
	struct net_buf *status = track(udc_ctrl_status_alloc(&dev, 0x80));
	struct net_buf *next = track(udc_ctrl_setup_alloc(&dev));

	enqueue(status);
	enqueue(next);
	tick();
	zassert_equal(hw.tx_count, 0, "Status IN must not queue a data-stage ZLP");
	zassert_true(hw.ack_end[0]);
	zassert_equal(hw.acks[0], acks + 1);
	setup_packet(true, 18);
	hw.next.tx = BIT(0);
	tick();
	zassert_equal(completions(status, 0), 1);
	zassert_equal(completions(next, 0), 1);
	zassert_equal(priv.phase, WS9652A_DATA_IN);
}

ZTEST(udc_ws9652a_api, test_no_data_status_defers_ack_and_waits_for_host)
{
	unsigned int acks = hw.acks[0];
	struct net_buf *setup = setup_request(false, 0);

	zassert_equal(hw.acks[0], acks);
	zassert_true(priv.ep0_pending);
	/* RXREADY still describes the consumed SETUP, not another packet. */
	hw.next.ep0_csr = WS9652A_USB_EP0_RX_READY;
	hw.next.tx = BIT(0);
	tick();
	zassert_equal(completions(setup, 0), 1);
	zassert_false(hw.stalled[0]);
	struct net_buf *status = track(udc_ctrl_status_alloc(&dev, 0x80));

	enqueue(status);
	tick();
	zassert_equal(hw.acks[0], acks + 1);
	zassert_true(hw.ack_end[0]);
	zassert_equal(hw.tx_count, 0);
	zassert_true(priv.in[0].active);
	zassert_false(priv.ep0_pending);
	zassert_equal(completions(status, 0), 0);
	/* A controller event alone is insufficient while DATAEND is set. */
	hw.next.ep0_csr = WS9652A_USB_EP0_DATA_END;
	tx_ack(0);
	zassert_equal(completions(status, 0), 0);
	hw.next.ep0_csr = 0;
	tick();
	zassert_equal(completions(status, 0), 0, "Require a completion interrupt too");
	tx_ack(0);
	zassert_equal(completions(status, 0), 1);
	zassert_equal(priv.phase, WS9652A_SETUP);
}

ZTEST(udc_ws9652a_api, test_new_setup_preempts_pending_status_ack)
{
	setup_request(false, 0);
	struct net_buf *status = track(udc_ctrl_status_alloc(&dev, 0x80));
	struct net_buf *next = track(udc_ctrl_setup_alloc(&dev));

	enqueue(status);
	enqueue(next);
	unsigned int acks = hw.acks[0];

	hw.next.ep0_csr = WS9652A_USB_EP0_SETUP_END;
	tick();
	zassert_equal(completions(status, -ECONNRESET), 1);
	zassert_equal(hw.acks[0], acks);
	zassert_equal(hw.tx_count, 0);
	zassert_false(priv.ep0_pending);
	setup_packet(true, 18);
	tick();
	zassert_equal(completions(next, 0), 1);
	zassert_equal(priv.phase, WS9652A_DATA_IN);
}

ZTEST(udc_ws9652a_api, test_rejected_no_data_request_stalls_without_status_ack)
{
	setup_request(false, 0);
	zassert_ok(udc_ep_set_halt(&dev, 0));
	zassert_true(hw.stalled[0]);
	zassert_false(hw.ack_end[0]);
	zassert_false(priv.ep0_pending);
	zassert_equal(hw.tx_count, 0);
	hw.next.ep0_csr = WS9652A_USB_EP0_SENT_STALL;
	tick();
	setup_request(true, 18);
	zassert_equal(priv.phase, WS9652A_DATA_IN);
	zassert_false(hw.stalled[0]);
}

ZTEST(udc_ws9652a_api, test_reset_cancels_status_without_releasing_old_setup)
{
	setup_request(false, 0);
	struct net_buf *status = track(udc_ctrl_status_alloc(&dev, 0x80));

	enqueue(status);
	unsigned int acks = hw.acks[0];

	reset_bus();
	zassert_equal(completions(status, -ECONNRESET), 1);
	zassert_equal(hw.acks[0], acks);
	zassert_equal(hw.tx_count, 0);
	zassert_false(priv.ep0_pending);
	zassert_equal(priv.phase, WS9652A_SETUP);
	zassert_true(priv.out[0].active);
}

ZTEST(udc_ws9652a_api, test_cancelled_status_does_not_redeliver_consumed_setup)
{
	struct net_buf *setup = setup_request(false, 0);
	struct net_buf *status = track(udc_ctrl_status_alloc(&dev, 0x80));
	struct net_buf *next = track(udc_ctrl_setup_alloc(&dev));

	enqueue(status);
	enqueue(next);
	unsigned int acks = hw.acks[0];

	zassert_ok(udc_ep_dequeue(&dev, 0x80));
	zassert_equal(completions(status, -ECONNABORTED), 1);
	zassert_equal(priv.phase, WS9652A_STATUS_IN);
	zassert_true(priv.ep0_pending);
	hw.next.ep0_csr = WS9652A_USB_EP0_RX_READY;
	tick();
	zassert_equal(completions(setup, 0), 1);
	zassert_equal(completions(next, 0), 0);
	zassert_equal(hw.acks[0], acks);
	struct net_buf *replacement = track(udc_ctrl_status_alloc(&dev, 0x80));

	enqueue(replacement);
	tick();
	zassert_true(hw.ack_end[0]);
	tx_ack(0);
	zassert_equal(completions(replacement, 0), 1);
}

ZTEST(udc_ws9652a_api, test_setup_end_aborts_in_and_preserves_next_setup)
{
	setup_request(true, 128);
	struct net_buf *old = request(0x80, 128, true);
	struct net_buf *next = track(udc_ctrl_setup_alloc(&dev));

	enqueue(old);
	enqueue(next);
	tick();
	setup_packet(false, 0);
	hw.next.ep0_csr |= WS9652A_USB_EP0_SETUP_END;
	hw.next.tx |= BIT(0);
	tick();
	zassert_equal(completions(old, -ECONNRESET), 1);
	zassert_equal(completions(next, 0), 1);
	zassert_equal(hw.tx_count, 1);
	zassert_equal(hw.setup_acks, 1);
	zassert_equal(priv.phase, WS9652A_STATUS_IN);
}

ZTEST(udc_ws9652a_api, test_bulk_in_standalone_zlp_completes_then_advances_queue)
{
	enable_bulk();
	struct net_buf *empty = request(BULK_IN, 0, true);
	struct net_buf *next = request(BULK_IN, 64, true);

	enqueue(empty);
	enqueue(next);
	tick();
	zassert_equal(hw.tx_count, 1);
	zassert_equal(hw.active_tx_mask, 0, "A queued request is not active before TX starts");
	zassert_equal(hw.tx[0].ep, 2);
	zassert_equal(hw.tx[0].size, 0);
	zassert_false(hw.tx[0].data_end);
	zassert_equal(completions(empty, 0), 0);
	zassert_equal(completions(next, 0), 0);

	/* An empty request still needs a hardware completion before the next one. */
	tick();
	zassert_equal(hw.active_tx_mask, BIT(2));
	zassert_equal(hw.tx_count, 1);
	zassert_equal(completions(empty, 0), 0);
	zassert_equal(completions(next, 0), 0);
	tx_ack(2);
	zassert_equal(hw.active_tx_mask, BIT(2));
	zassert_equal(completions(empty, 0), 1);
	zassert_equal(completions(next, 0), 0);
	zassert_equal(hw.tx_count, 2);
	zassert_equal(hw.tx[1].ep, 2);
	zassert_equal(hw.tx[1].size, 64);
	for (size_t i = 0; i < hw.tx[1].size; i++) {
		zassert_equal(hw.tx[1].data[i], i);
	}

	tx_ack(2);
	zassert_equal(hw.active_tx_mask, BIT(2));
	zassert_equal(completions(empty, 0), 1);
	zassert_equal(completions(next, 0), 1);
	zassert_false(priv.in[2].active);
	zassert_false(priv.in[2].cfg.stat.busy);
	/* A late event cannot complete either request twice or emit another ZLP. */
	tx_ack(2);
	zassert_equal(hw.active_tx_mask, 0);
	zassert_equal(completions(empty, 0), 1);
	zassert_equal(completions(next, 0), 1);
	zassert_equal(hw.tx_count, 2);
}

ZTEST(udc_ws9652a_api, test_bulk_in_multipacket_short_suppresses_extra_zlp)
{
	enable_bulk();
	struct net_buf *buf = request(BULK_IN, 70, true);

	udc_get_buf_info(buf)->zlp = true;
	enqueue(buf);
	tick();
	tx_ack(2);
	zassert_equal(hw.tx_count, 2);
	zassert_equal(hw.tx[1].size, 6);
	zassert_equal(hw.tx[1].data[0], 64);
	tx_ack(2);
	zassert_equal(completions(buf, 0), 1);
	zassert_equal(hw.tx_count, 2);
}

ZTEST(udc_ws9652a_api, test_bulk_out_short_and_full_rearm_next_request)
{
	enable_bulk();
	struct net_buf *first = request(BULK_OUT, 128, false);
	struct net_buf *second = request(BULK_OUT, 64, false);
	uint8_t packet[64];

	memset(packet, 0xa5, sizeof(packet));
	enqueue(first);
	enqueue(second);
	tick();
	receive(2, packet, 64);
	tick();
	zassert_equal(hw.active_tx_mask, 0, "An active OUT request must not be polled as TX");
	zassert_equal(completions(first, 0), 0);
	receive(2, "short", 5);
	tick();
	zassert_equal(completions(first, 0), 1);
	zassert_equal(first->len, 69);
	zassert_mem_equal(first->data, packet, 64);
	zassert_mem_equal(first->data + 64, "short", 5);
	zassert_true(priv.out[2].active);
	receive(2, packet, 64);
	tick();
	zassert_equal(completions(second, 0), 1);
	zassert_equal(second->len, 64);
	zassert_false(priv.out[2].active);
	zassert_equal(hw.arms[2], 3);
}

ZTEST(udc_ws9652a_api, test_bulk_out_overflow_halts_without_copying)
{
	enable_bulk();
	struct net_buf *buf = request(BULK_OUT, 8, false);

	enqueue(buf);
	tick();
	receive(2, "too-long-packet", 15);
	tick();
	zassert_equal(completions(buf, -EMSGSIZE), 1);
	zassert_equal(buf->len, 0);
	zassert_true(hw.stalled[BULK_OUT]);
	zassert_true(priv.out[2].cfg.stat.halted);
	zassert_ok(udc_ep_clear_halt(&dev, BULK_OUT));
	zassert_false(priv.out[2].cfg.stat.halted);
}

ZTEST(udc_ws9652a_api, test_bulk_out_ack_before_rearm_preserves_65th_byte)
{
	enable_bulk();
	struct net_buf *buf = request(BULK_OUT, 128, false);
	uint8_t packet[64];

	memset(packet, 'x', sizeof(packet));
	enqueue(buf);
	tick();
	receive(2, packet, sizeof(packet));
	receive_bulk_at_ack("\r", 1);
	tick();
	zassert_equal(buf->len, 64);
	zassert_equal(completions(buf, 0), 0);
	zassert_equal(hw.bulk_at_ack.stranded, 0);
	zassert_false(hw.bulk_at_ack.fifo_waiting);
	zassert_equal(hw.count[2], 1);
	tick();
	zassert_equal(completions(buf, 0), 1);
	zassert_equal(buf->len, 65);
	zassert_mem_equal(buf->data, packet, sizeof(packet));
	zassert_equal(buf->data[64], '\r');
}

ZTEST(udc_ws9652a_api, test_bulk_out_ack_before_queued_request_preserves_next_packet)
{
	enable_bulk();
	struct net_buf *first = request(BULK_OUT, 64, false);
	struct net_buf *next = request(BULK_OUT, 64, false);
	uint8_t packet[64];

	memset(packet, 'x', sizeof(packet));
	enqueue(first);
	enqueue(next);
	tick();
	receive(2, packet, sizeof(packet));
	receive_bulk_at_ack("\r", 1);
	tick();
	zassert_equal(completions(first, 0), 1);
	zassert_equal(first->len, 64);
	zassert_mem_equal(first->data, packet, sizeof(packet));
	zassert_equal(completions(next, 0), 0);
	zassert_equal(next->len, 0);
	zassert_equal(hw.bulk_at_ack.stranded, 0);
	zassert_false(hw.bulk_at_ack.fifo_waiting);
	zassert_true(priv.out[2].active);
	zassert_equal_ptr(udc_buf_peek(&priv.out[2].cfg), next);
	tick();
	zassert_equal(completions(first, 0), 1);
	zassert_equal(completions(next, 0), 1);
	zassert_equal(next->len, 1);
	zassert_equal(next->data[0], '\r');
	zassert_false(priv.out[2].active);
}

ZTEST(udc_ws9652a_api, test_reset_cancels_requests_once_and_preserves_setup)
{
	enable_bulk();
	struct net_buf *in = request(BULK_IN, 128, true);
	struct net_buf *out = request(BULK_OUT, 128, false);
	struct net_buf *setup = track(udc_ctrl_setup_alloc(&dev));

	enqueue(in);
	enqueue(out);
	enqueue(setup);
	tick();
	reset_bus();
	zassert_equal(completions(in, -ECONNRESET), 1);
	zassert_equal(completions(out, -ECONNRESET), 1);
	zassert_equal(completions(setup, 0), 0);
	zassert_false(priv.in[2].configured);
	zassert_false(priv.out[2].configured);
	setup_packet(false, 0);
	tick();
	zassert_equal(completions(setup, 0), 1);
	zassert_equal(events[event_count - 2].type, UDC_EVT_RESET);
}

ZTEST(udc_ws9652a_api, test_dequeue_cancels_all_and_does_not_reset_data_toggle)
{
	enable_bulk();
	struct net_buf *first = request(BULK_IN, 128, true);
	struct net_buf *second = request(BULK_IN, 64, true);

	enqueue(first);
	enqueue(second);
	tick();
	unsigned int configs = hw.configs[BULK_IN];

	zassert_ok(udc_ep_dequeue(&dev, BULK_IN));
	zassert_equal(completions(first, -ECONNABORTED), 1);
	zassert_equal(completions(second, -ECONNABORTED), 1);
	zassert_equal(hw.configs[BULK_IN], configs, "dequeue must preserve DATA toggle");
	zassert_equal(hw.cancels[BULK_IN], 1);
	struct net_buf *fresh = request(BULK_IN, 4, true);

	enqueue(fresh);
	tick();
	tx_ack(2);
	zassert_equal(completions(fresh, 0), 1);
	zassert_ok(udc_ep_disable(&dev, BULK_IN));
	zassert_ok(udc_ep_dequeue(&dev, BULK_IN));
	zassert_false(priv.in[2].configured);
	zassert_ok(udc_ep_enable(&dev, BULK_IN, USB_EP_TYPE_BULK, 64, 0));
	zassert_true(priv.in[2].configured);
}

ZTEST(udc_ws9652a_api, test_disconnect_cancels_without_fabricating_vbus_event)
{
	enable_bulk();
	struct net_buf *buf = request(BULK_IN, 64, true);

	enqueue(buf);
	tick();
	hw.next.wrapper = WS9652A_USB_WRAPPER_DISCONNECT;
	tick();
	zassert_equal(completions(buf, -ECONNRESET), 1);
	zassert_true(priv.awaiting_reset);
	zassert_true(hw.connected);
	for (size_t i = 0; i < event_count; i++) {
		zassert_not_equal(events[i].type, UDC_EVT_VBUS_REMOVED);
	}
	reset_bus();
	zassert_false(priv.awaiting_reset);
}

ZTEST(udc_ws9652a_api, test_hardware_errors_complete_with_error)
{
	enable_bulk();
	struct net_buf *start = request(BULK_IN, 64, true);

	hw.tx_error = -EIO;
	enqueue(start);
	tick();
	zassert_equal(completions(start, -EIO), 1);
	hw.tx_error = 0;
	struct net_buf *active = request(BULK_IN, 64, true);

	enqueue(active);
	tick();
	hw.next.tx = BIT(2);
	hw.next.tx_error = BIT(2);
	tick();
	zassert_equal(completions(active, -EIO), 1);
	zassert_true(hw.stalled[BULK_IN]);
	struct net_buf *rx = request(BULK_OUT, 64, false);

	hw.rx_error = -EIO;
	enqueue(rx);
	tick();
	zassert_equal(completions(rx, -EIO), 1);
	hw.rx_error = 0;
}

ZTEST(udc_ws9652a_api, test_control_out_rejects_more_than_requested)
{
	setup_request(false, 7);
	struct net_buf *out = track(udc_ctrl_data_alloc(&dev, 0, 7));

	enqueue(out);
	receive(0, "overflow", 8);
	tick();
	zassert_equal(completions(out, -EMSGSIZE), 1);
	zassert_equal(out->len, 0);
	zassert_true(hw.stalled[0]);
	zassert_equal(priv.phase, WS9652A_SETUP);
}

ZTEST(udc_ws9652a_api, test_setup_end_recovers_when_previous_out_waited_for_buffer)
{
	setup_request(false, 7);
	receive(0, "payload", 7);
	tick();
	zassert_true(priv.ep0_pending);
	zassert_false(priv.out[0].active);
	struct net_buf *next = track(udc_ctrl_setup_alloc(&dev));

	enqueue(next);
	hw.next.ep0_csr = WS9652A_USB_EP0_SETUP_END | WS9652A_USB_EP0_RX_READY;
	tick();
	zassert_equal(completions(next, 0), 0);
	zassert_true(priv.out[0].active);
	zassert_false(priv.ep0_pending);
	/* SETUPEND processing replaces the stale data DMA. The still-pending
	 * SETUP packet becomes available only after that receive is armed.
	 */
	setup_packet(false, 0);
	tick();
	zassert_equal(completions(next, 0), 1);
	zassert_equal(next->len, 8);
	zassert_equal(priv.phase, WS9652A_STATUS_IN);
}

ZTEST(udc_ws9652a_api, test_setup_rearm_error_is_reported_without_releasing_packet)
{
	struct net_buf *setup = track(udc_ctrl_setup_alloc(&dev));

	enqueue(setup);
	setup_packet(true, 18);
	unsigned int acks = hw.acks[0];

	hw.rx_error = -EIO;
	tick();
	zassert_equal(completions(setup, 0), 1);
	zassert_equal(events[event_count - 1].type, UDC_EVT_ERROR);
	zassert_equal(events[event_count - 1].status, -EIO);
	zassert_equal(hw.acks[0], acks);
	hw.rx_error = 0;
}

ZTEST(udc_ws9652a_api, test_clear_halt_restarts_unacknowledged_request)
{
	enable_bulk();
	struct net_buf *buf = request(BULK_IN, 64, true);

	enqueue(buf);
	tick();
	zassert_equal(hw.tx_count, 1);
	zassert_ok(udc_ep_set_halt(&dev, BULK_IN));
	tx_ack(2);
	zassert_equal(completions(buf, 0), 0);
	zassert_equal(hw.tx_count, 1);
	zassert_ok(udc_ep_clear_halt(&dev, BULK_IN));
	tick();
	zassert_equal(hw.tx_count, 2);
	zassert_mem_equal(hw.tx[0].data, hw.tx[1].data, 64);
	tx_ack(2);
	zassert_equal(completions(buf, 0), 1);
}

ZTEST(udc_ws9652a_api, test_bulk_rx_error_does_not_deliver_untrusted_payload)
{
	enable_bulk();
	struct net_buf *buf = request(BULK_OUT, 64, false);

	enqueue(buf);
	tick();
	receive(2, "invalid", 7);
	hw.next.rx_error = BIT(2);
	tick();
	zassert_equal(completions(buf, -EIO), 1);
	zassert_equal(buf->len, 0);
	zassert_true(hw.stalled[BULK_OUT]);
}

ZTEST(udc_ws9652a_api, test_dequeuing_setup_receiver_preserves_control_in)
{
	setup_request(true, 128);
	struct net_buf *in = request(0x80, 128, true);
	struct net_buf *setup = track(udc_ctrl_setup_alloc(&dev));

	enqueue(in);
	enqueue(setup);
	tick();
	zassert_ok(udc_ep_dequeue(&dev, 0));
	zassert_equal(completions(setup, -ECONNABORTED), 1);
	zassert_equal(priv.phase, WS9652A_DATA_IN);
	tx_ack(0);
	zassert_equal(hw.tx_count, 2);
	zassert_true(hw.tx[1].data_end);
	tx_ack(0);
	zassert_equal(completions(in, 0), 1);
	setup_packet(false, 0);
	tick();
	zassert_true(common.setup_pending);
	struct net_buf *replacement = track(udc_ctrl_setup_alloc(&dev));

	enqueue(replacement);
	zassert_equal(completions(replacement, 0), 1);
}

static struct net_buf *active_status(void)
{
	setup_request(false, 0);
	struct net_buf *status = track(udc_ctrl_status_alloc(&dev, USB_CONTROL_EP_IN));

	enqueue(status);
	tick();
	zassert_true(priv.in[0].active);
	return status;
}

static void status_tick(uint32_t csr, int64_t elapsed)
{
	fake_now += elapsed;
	hw.next.ep0_csr = csr;
	tick();
}

ZTEST(udc_ws9652a_api, test_stalled_status_keeps_connection_and_requests_until_bus_reset)
{
	struct net_buf *status = active_status();
	struct net_buf *setup = track(udc_ctrl_setup_alloc(&dev));

	enqueue(setup);
	enable_bulk();
	struct net_buf *in = request(BULK_IN, 128, true);
	struct net_buf *out = request(BULK_OUT, 64, false);

	enqueue(in);
	enqueue(out);
	status_tick(WS9652A_USB_EP0_DATA_END, 0);
	unsigned int resets = hw.resets;
	unsigned int inits = hw.inits;
	unsigned int disables = hw.hw_disables;
	unsigned int connects = hw.connects;
	unsigned int reset_attempts = hw.reset_attempts;

	/* Cross the former three-second timeout, then leave status unchanged
	 * for a minute. Neither inactivity nor unrelated bulk progress may
	 * disconnect the controller or cancel its queued requests.
	 */
	status_tick(WS9652A_USB_EP0_DATA_END, 3000);
	hw.next.tx = BIT(2);
	status_tick(WS9652A_USB_EP0_DATA_END, 60000);
	hw.next.tx = BIT(2);
	status_tick(WS9652A_USB_EP0_DATA_END, 3000);
	zassert_true(hw.connected);
	zassert_true(priv.hw_initialized);
	zassert_false(priv.awaiting_reset);
	zassert_equal(hw.resets, resets);
	zassert_equal(hw.inits, inits);
	zassert_equal(hw.hw_disables, disables);
	zassert_equal(hw.connects, connects);
	zassert_equal(hw.reset_attempts, reset_attempts);
	zassert_true(priv.in[0].active);
	zassert_equal_ptr(udc_buf_peek(&priv.in[0].cfg), status);
	zassert_equal_ptr(udc_buf_peek(&priv.out[2].cfg), out);
	zassert_equal(completions(status, 0), 0);
	zassert_equal(completions(out, 0), 0);
	zassert_equal(completions(in, 0), 1);
	zassert_equal(completions(setup, 0), 0);

	reset_bus();
	zassert_equal(hw.resets, resets + 1);
	zassert_equal(hw.reset_attempts, reset_attempts + 1);
	zassert_equal(completions(status, -ECONNRESET), 1);
	zassert_equal(completions(out, -ECONNRESET), 1);
	zassert_equal(completions(in, 0), 1);
	setup_packet(true, 18);
	tick();
	zassert_equal(completions(setup, 0), 1);
}

ZTEST(udc_ws9652a_api, test_stalled_status_with_rxready_does_not_reconnect)
{
	struct net_buf *status = active_status();
	const uint8_t packet[8] = {2, 1, 0, 0, 0x82, 0, 0, 0};
	uint32_t csr = WS9652A_USB_EP0_DATA_END | WS9652A_USB_EP0_RX_READY;
	unsigned int inits = hw.inits;
	unsigned int disables = hw.hw_disables;
	unsigned int connects = hw.connects;
	unsigned int resets = hw.resets;

	receive(0, packet, sizeof(packet));
	status_tick(csr, 0);
	zassert_true(priv.ep0_pending);
	zassert_false(priv.out[0].active);
	status_tick(csr, 3000);
	status_tick(csr, 60000);
	zassert_true(hw.connected);
	zassert_true(priv.hw_initialized);
	zassert_false(priv.awaiting_reset);
	zassert_equal(hw.inits, inits);
	zassert_equal(hw.hw_disables, disables);
	zassert_equal(hw.connects, connects);
	zassert_equal(hw.resets, resets);
	zassert_true(priv.in[0].active);
	zassert_equal_ptr(udc_buf_peek(&priv.in[0].cfg), status);
	zassert_equal(completions(status, 0), 0);
	zassert_true(priv.ep0_pending);
	zassert_mem_equal(priv.out[0].packet, packet, sizeof(packet));
}

ZTEST(udc_ws9652a_api, test_suspended_status_completes_after_resume)
{
	struct net_buf *status = active_status();

	hw.next.bus = WS9652A_USB_BUS_SUSPEND;
	status_tick(WS9652A_USB_EP0_DATA_END, 0);
	zassert_true(udc_is_suspended(&dev));
	status_tick(WS9652A_USB_EP0_DATA_END, 6000);
	zassert_true(hw.connected);
	hw.next.bus = WS9652A_USB_BUS_RESUME;
	status_tick(WS9652A_USB_EP0_DATA_END, 0);
	zassert_false(udc_is_suspended(&dev));
	tx_ack(0);
	zassert_equal(completions(status, 0), 1);
}

ZTEST(udc_ws9652a_api, test_explicit_disable_cancels_status_and_enable_waits_for_reset)
{
	struct net_buf *status = active_status();

	zassert_ok(udc_disable(&dev));
	zassert_false(priv.reset_notify_pending);
	zassert_false(hw.connected);
	zassert_false(priv.hw_initialized);
	zassert_equal(completions(status, -ECONNABORTED), 1);
	unsigned int connects = hw.connects;

	fake_now += 60000;
	/* The real owner only polls while the public controller is enabled. */
	if (udc_is_enabled(&dev)) {
		tick();
	}
	zassert_equal(hw.connects, connects);
	zassert_false(hw.connected);
	zassert_ok(udc_enable(&dev));
	zassert_true(hw.connected);
	zassert_true(priv.awaiting_reset);
	reset_bus();
	zassert_false(priv.awaiting_reset);
	zassert_equal(completions(status, -ECONNABORTED), 1);
}

ZTEST(udc_ws9652a_api, test_reset_notification_retry_holds_setup_without_recancelling)
{
	enable_bulk();
	struct net_buf *in = request(BULK_IN, 64, true);
	struct net_buf *setup = track(udc_ctrl_setup_alloc(&dev));

	enqueue(in);
	enqueue(setup);
	tick();
	hw.reset_rejects = 2;
	reset_bus();
	unsigned int resets = hw.resets;
	unsigned int arms = hw.arms[0];

	zassert_true(priv.reset_notify_pending);
	zassert_true(priv.awaiting_reset);
	zassert_equal(completions(in, -ECONNRESET), 1);
	setup_packet(true, 18);
	tick();
	zassert_true(priv.reset_notify_pending);
	zassert_equal(completions(setup, 0), 0);
	hw.next.ep0_csr = WS9652A_USB_EP0_RX_READY;
	tick();
	zassert_false(priv.reset_notify_pending);
	zassert_false(priv.awaiting_reset);
	zassert_equal(completions(setup, 0), 0, "RESET publication must precede SETUP delivery");
	zassert_equal(hw.resets, resets);
	zassert_equal(hw.arms[0], arms);
	zassert_equal(completions(in, -ECONNRESET), 1);
	hw.next.ep0_csr = WS9652A_USB_EP0_RX_READY;
	tick();
	zassert_equal(completions(setup, 0), 1);
}

ZTEST(udc_ws9652a_api, test_disconnect_discards_dropped_reset_notification)
{
	hw.reset_rejects = 2;
	reset_bus();
	zassert_true(priv.reset_notify_pending);
	unsigned int attempts = hw.reset_attempts;

	hw.next.wrapper = WS9652A_USB_WRAPPER_DISCONNECT;
	tick();
	zassert_false(priv.reset_notify_pending);
	tick();
	zassert_equal(hw.reset_attempts, attempts);
	zassert_true(priv.awaiting_reset);
}

ZTEST(udc_ws9652a_api, test_endpoint_callbacks_do_not_rearm_disabled_controller)
{
	active_status();
	enable_bulk();
	zassert_ok(udc_disable(&dev));
	unsigned int arms = hw.arms[0];
	unsigned int cancels = hw.cancels[0];
	unsigned int disables = hw.disables[BULK_IN];
	unsigned int configs = hw.configs[BULK_IN];

	zassert_ok(udc_ep_dequeue(&dev, USB_CONTROL_EP_OUT));
	zassert_equal(udc_ep_set_halt(&dev, USB_CONTROL_EP_OUT), -EPERM);
	zassert_equal(udc_ep_clear_halt(&dev, USB_CONTROL_EP_OUT), -EPERM);
	zassert_ok(udc_ep_disable(&dev, BULK_IN));
	zassert_equal(udc_ep_enable(&dev, BULK_IN, USB_EP_TYPE_BULK, 64, 0), -EPERM);
	zassert_equal(udc_set_address(&dev, 17), -EPERM);
	zassert_equal(hw.arms[0], arms);
	zassert_equal(hw.cancels[0], cancels);
	zassert_equal(hw.disables[BULK_IN], disables);
	zassert_equal(hw.configs[BULK_IN], configs);
	zassert_false(hw.connected);
}

ZTEST(udc_ws9652a_api, test_enable_rearm_error_keeps_pullup_off_until_explicit_retry)
{
	zassert_ok(udc_disable(&dev));
	hw.rx_error = -EIO;
	zassert_equal(udc_enable(&dev), -EIO);
	zassert_false(hw.connected);
	zassert_false(priv.hw_initialized);
	zassert_false(udc_is_enabled(&dev));
	unsigned int inits = hw.inits;

	fake_now += 60000;
	if (udc_is_enabled(&dev)) {
		tick();
	}
	zassert_equal(hw.inits, inits);
	hw.rx_error = 0;
	zassert_ok(udc_enable(&dev));
	zassert_true(hw.connected);
	zassert_true(priv.awaiting_reset);
}

ZTEST_SUITE(udc_ws9652a_api, NULL, NULL, before, after, NULL);
