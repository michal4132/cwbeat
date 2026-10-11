/* SPDX-License-Identifier: Apache-2.0 */
#define DT_DRV_COMPAT zgmicro_ws9652a_udc

#include "udc_common.h"
#include "ws9652a_usb_hw.h"

#include <errno.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(udc_ws9652a, CONFIG_UDC_DRIVER_LOG_LEVEL);

BUILD_ASSERT(CONFIG_UDC_WS9652A_THREAD_PRIORITY >= 0 &&
		     CONFIG_UDC_WS9652A_THREAD_PRIORITY < CONFIG_NUM_PREEMPT_PRIORITIES,
	     "WS9652A USB worker must have a valid preemptible priority");

enum ws9652a_control_phase {
	WS9652A_SETUP,
	WS9652A_DATA_IN,
	WS9652A_DATA_OUT,
	WS9652A_STATUS_IN,
};

struct ws9652a_ep {
	struct udc_ep_config cfg;
	/* The DMA engine requires word alignment. Requests may be unaligned. */
	uint8_t packet[WS9652A_USB_MAX_PACKET] __aligned(4);
	uint16_t packet_len;
	bool active;
	bool configured;
};

struct ws9652a_udc_config {
	struct ws9652a_usb_hw hw;
	k_thread_stack_t *stack;
	size_t stack_size;
};

struct ws9652a_udc_data {
	struct k_thread thread;
	struct k_sem wake;
	struct ws9652a_ep in[WS9652A_USB_EP_COUNT];
	struct ws9652a_ep out[WS9652A_USB_EP_COUNT];
	enum ws9652a_control_phase phase;
	uint16_t control_remaining;
	bool ep0_pending;
	bool hw_initialized;
	bool awaiting_reset;
	bool reset_notify_pending;
};

static const struct ws9652a_usb_hw *ws9652a_hw(const struct device *dev)
{
	const struct ws9652a_udc_config *config = dev->config;

	return &config->hw;
}

static struct ws9652a_ep *ws9652a_ep(struct udc_ep_config *cfg)
{
	return CONTAINER_OF(cfg, struct ws9652a_ep, cfg);
}

static void ws9652a_complete(const struct device *dev, struct ws9652a_ep *ep, int err)
{
	struct net_buf *buf = udc_buf_get(&ep->cfg);

	ep->active = false;
	udc_ep_set_busy(&ep->cfg, false);
	if (buf != NULL) {
		udc_submit_ep_event(dev, buf, err);
	}
}

static void ws9652a_cancel(const struct device *dev, struct ws9652a_ep *ep, int err,
			   bool keep_setup)
{
	struct net_buf *setup = NULL;
	struct net_buf *buf;

	if (!ep->cfg.caps.in && !ep->cfg.caps.out) {
		return;
	}
	ep->active = false;
	udc_ep_set_busy(&ep->cfg, false);
	while ((buf = udc_buf_get(&ep->cfg)) != NULL) {
		if (keep_setup && udc_get_buf_info(buf)->setup && setup == NULL) {
			setup = buf;
		} else {
			udc_submit_ep_event(dev, buf, err);
		}
	}
	if (setup != NULL) {
		udc_buf_put(&ep->cfg, setup);
	}
}

static int ws9652a_arm_rx(const struct device *dev, struct ws9652a_ep *ep)
{
	int ret = ws9652a_usb_hw_rx_arm(ws9652a_hw(dev), USB_EP_GET_IDX(ep->cfg.addr), ep->packet,
					ep->cfg.mps);

	if (ret == 0) {
		ep->active = true;
	}
	return ret;
}

static void ws9652a_start_tx(const struct device *dev, struct ws9652a_ep *ep)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct net_buf *buf = udc_buf_peek(&ep->cfg);
	uint8_t index = USB_EP_GET_IDX(ep->cfg.addr);
	bool data_end;
	int ret;

	if (buf == NULL || ep->active || !ep->configured || ep->cfg.stat.halted) {
		return;
	}
	if (index == 0 && priv->phase != WS9652A_DATA_IN && priv->phase != WS9652A_STATUS_IN) {
		return;
	}
	if (index == 0 && priv->phase == WS9652A_STATUS_IN) {
		/* Keep the preceding RXREADY asserted until the stack accepts the
		 * request. RXACK|DATAEND makes the core send status IN; TXREADY is
		 * for a data-stage packet and must not be queued separately here.
		 */
		if (!priv->ep0_pending) {
			return;
		}
		if (buf->len != 0) {
			ws9652a_complete(dev, ep, -EINVAL);
			return;
		}
		ret = ws9652a_arm_rx(dev, &priv->out[0]);
		if (ret != 0) {
			ws9652a_complete(dev, ep, ret);
			return;
		}
		priv->ep0_pending = false;
		ep->packet_len = 0;
		udc_ep_buf_clear_zlp(buf);
		ep->active = true;
		udc_ep_set_busy(&ep->cfg, true);
		ws9652a_usb_hw_rx_ack(ws9652a_hw(dev), 0, true);
		return;
	}

	ep->packet_len = MIN(buf->len, ep->cfg.mps);
	if (ep->packet_len != 0U) {
		memcpy(ep->packet, buf->data, ep->packet_len);
	}
	/* A short packet already terminates a transfer, including a queued ZLP. */
	if (ep->packet_len < ep->cfg.mps) {
		udc_ep_buf_clear_zlp(buf);
	}
	data_end = index == 0 && ep->packet_len == buf->len && !udc_ep_buf_has_zlp(buf);
	ret = ws9652a_usb_hw_tx_start(ws9652a_hw(dev), index, ep->packet, ep->packet_len, data_end);
	if (ret != 0) {
		ws9652a_complete(dev, ep, ret);
		return;
	}
	ep->active = true;
	udc_ep_set_busy(&ep->cfg, true);
}

static void ws9652a_tx_done(const struct device *dev, struct ws9652a_ep *ep, int err)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct net_buf *buf = udc_buf_peek(&ep->cfg);

	if (!ep->active || buf == NULL) {
		return;
	}
	ep->active = false;
	if (err != 0) {
		ws9652a_usb_hw_ep_disable(ws9652a_hw(dev), ep->cfg.addr);
		ws9652a_usb_hw_stall(ws9652a_hw(dev), ep->cfg.addr, true);
		ep->cfg.stat.halted = true;
		ws9652a_complete(dev, ep, err);
		return;
	}

	net_buf_pull(buf, ep->packet_len);
	if (buf->len != 0 || udc_ep_buf_has_zlp(buf)) {
		ws9652a_start_tx(dev, ep);
		return;
	}
	if (USB_EP_GET_IDX(ep->cfg.addr) == 0) {
		/* DATAEND lets the core acknowledge control-read status OUT.
		 * SET_ADDRESS is applied by the stack after this status-IN event.
		 */
		priv->phase = WS9652A_SETUP;
	}
	ws9652a_complete(dev, ep, 0);
}

static void ws9652a_ep0_rearm_ack(const struct device *dev, bool data_end)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	int ret;

	priv->ep0_pending = false;
	ret = ws9652a_arm_rx(dev, &priv->out[0]);
	if (ret != 0) {
		udc_submit_event(dev, UDC_EVT_ERROR, ret);
		return;
	}
	/* Prepare storage before releasing the received packet to the core. */
	ws9652a_usb_hw_rx_ack(ws9652a_hw(dev), 0, data_end);
}

static void ws9652a_ep0_stall(const struct device *dev)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);

	if (priv->hw_initialized) {
		ws9652a_usb_hw_ep_disable(ws9652a_hw(dev), USB_CONTROL_EP_IN);
	}
	ws9652a_cancel(dev, &priv->in[0], -ECONNABORTED, false);
	ws9652a_cancel(dev, &priv->out[0], -ECONNABORTED, true);
	priv->phase = WS9652A_SETUP;
	priv->control_remaining = 0;
	if (priv->hw_initialized) {
		ws9652a_ep0_rearm_ack(dev, false);
		ws9652a_usb_hw_stall(ws9652a_hw(dev), USB_CONTROL_EP_OUT, true);
	} else {
		priv->ep0_pending = false;
	}
}

static void ws9652a_setup(const struct device *dev)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct ws9652a_ep *out = &priv->out[0];
	uint8_t setup[sizeof(struct usb_setup_packet)];

	if (out->packet_len != sizeof(setup)) {
		/* The core can report the automatically acknowledged status ZLP. */
		if (out->packet_len == 0) {
			ws9652a_ep0_rearm_ack(dev, true);
		} else {
			ws9652a_ep0_stall(dev);
		}
		return;
	}

	memcpy(setup, out->packet, sizeof(setup));
	ws9652a_usb_hw_ep_disable(ws9652a_hw(dev), USB_CONTROL_EP_IN);
	priv->in[0].active = false;
	priv->in[0].cfg.stat.halted = false;
	out->cfg.stat.halted = false;
	priv->control_remaining = sys_get_le16(setup + 6);
	priv->phase = priv->control_remaining == 0    ? WS9652A_STATUS_IN
		      : USB_REQTYPE_GET_DIR(setup[0]) ? WS9652A_DATA_IN
						      : WS9652A_DATA_OUT;
	/* The common helper cancels obsolete requests before handing over SETUP. */
	udc_setup_received(dev, setup);
	if (priv->phase != WS9652A_STATUS_IN) {
		ws9652a_ep0_rearm_ack(dev, false);
	}
}

static void ws9652a_ep0_out(const struct device *dev)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct ws9652a_ep *ep = &priv->out[0];
	struct net_buf *buf;
	bool done;

	if (!priv->ep0_pending) {
		return;
	}
	if (priv->phase == WS9652A_SETUP) {
		ws9652a_setup(dev);
		return;
	}
	if (priv->phase == WS9652A_STATUS_IN) {
		/* The final OUT packet or no-data SETUP is already consumed, but
		 * its RXREADY remains asserted until status IN is queued.
		 */
		return;
	}
	if (priv->phase != WS9652A_DATA_OUT) {
		ws9652a_ep0_stall(dev);
		return;
	}
	buf = udc_buf_peek(&ep->cfg);
	if (buf == NULL || udc_get_buf_info(buf)->setup) {
		/* Leave RXREADY asserted until the stack has queued the data buffer. */
		return;
	}
	if (ep->packet_len > ep->cfg.mps || ep->packet_len > net_buf_tailroom(buf) ||
	    ep->packet_len > priv->control_remaining) {
		ws9652a_complete(dev, ep, -EMSGSIZE);
		ws9652a_ep0_stall(dev);
		return;
	}
	net_buf_add_mem(buf, ep->packet, ep->packet_len);
	priv->control_remaining -= ep->packet_len;
	done = ep->packet_len < ep->cfg.mps || priv->control_remaining == 0;
	if (done) {
		priv->phase = WS9652A_STATUS_IN;
		ws9652a_complete(dev, ep, 0);
		return;
	}
	ws9652a_ep0_rearm_ack(dev, false);
}

static void ws9652a_rx_done(const struct device *dev, struct ws9652a_ep *ep, int err)
{
	struct net_buf *buf = udc_buf_peek(&ep->cfg);
	uint8_t index = USB_EP_GET_IDX(ep->cfg.addr);
	uint16_t count;

	if (!ep->active || buf == NULL) {
		return;
	}
	ep->active = false;
	count = ws9652a_usb_hw_rx_count(ws9652a_hw(dev), index);
	if (err == 0 && (count > ep->cfg.mps || count > net_buf_tailroom(buf))) {
		err = -EMSGSIZE;
	}
	if (err != 0) {
		ws9652a_usb_hw_ep_disable(ws9652a_hw(dev), ep->cfg.addr);
		ws9652a_usb_hw_stall(ws9652a_hw(dev), ep->cfg.addr, true);
		ep->cfg.stat.halted = true;
		ws9652a_complete(dev, ep, err);
		return;
	}
	net_buf_add_mem(buf, ep->packet, count);
	if (count < ep->cfg.mps || net_buf_tailroom(buf) == 0) {
		ws9652a_complete(dev, ep, 0);
	}
	/* Retire this FIFO packet before arming DMA for the next one. Arming
	 * while RXREADY is held can merge the next packet into its RX count,
	 * then leave those trailing bytes without another receive event.
	 */
	ws9652a_usb_hw_rx_ack(ws9652a_hw(dev), index, false);
	if (udc_buf_peek(&ep->cfg) != NULL) {
		err = ws9652a_arm_rx(dev, ep);
		if (err != 0) {
			ws9652a_complete(dev, ep, err);
		} else {
			udc_ep_set_busy(&ep->cfg, true);
		}
	}
}

static void ws9652a_clear_transfers(const struct device *dev)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct udc_data *data = dev->data;

	priv->phase = WS9652A_SETUP;
	priv->control_remaining = 0;
	priv->ep0_pending = false;
	priv->awaiting_reset = true;
	priv->reset_notify_pending = false;
	data->setup_pending = false;
	data->setup_valid = false;
	for (unsigned int i = 0; i < WS9652A_USB_EP_COUNT; i++) {
		ws9652a_cancel(dev, &priv->in[i], -ECONNRESET, false);
		ws9652a_cancel(dev, &priv->out[i], -ECONNRESET, i == 0);
		priv->in[i].cfg.stat.halted = false;
		priv->out[i].cfg.stat.halted = false;
		/* The stack disables/re-enables non-control endpoints on reconfigure. */
		priv->in[i].configured = i == 0;
		priv->out[i].configured = i == 0;
	}
	if (udc_is_suspended(dev)) {
		udc_set_suspended(dev, false);
	}
}

static void ws9652a_notify_reset(const struct device *dev)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);

	/* Retry a dropped notification without repeating reset or cancellation. */
	if (udc_submit_event(dev, UDC_EVT_RESET, 0) == 0) {
		priv->reset_notify_pending = false;
		priv->awaiting_reset = false;
	}
}

static void ws9652a_reset(const struct device *dev, bool bus_reset)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);

	ws9652a_usb_hw_reset(ws9652a_hw(dev));
	ws9652a_clear_transfers(dev);
	if (bus_reset) {
		int ret = ws9652a_arm_rx(dev, &priv->out[0]);

		if (ret != 0) {
			udc_submit_event(dev, UDC_EVT_ERROR, ret);
		} else {
			priv->reset_notify_pending = true;
			ws9652a_notify_reset(dev);
		}
	}
}

/* Called with the UDC mutex held; API operations cannot race DMA cancellation. */
static void ws9652a_poll(const struct device *dev)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct ws9652a_usb_event event;
	uint8_t active_tx_mask = 0;
	bool new_setup = false;
	bool rx_rearmed = false;

	for (unsigned int i = 1; i < WS9652A_USB_EP_COUNT; i++) {
		if (priv->in[i].active) {
			active_tx_mask |= BIT(i);
		}
	}
	ws9652a_usb_hw_poll(ws9652a_hw(dev), &event, active_tx_mask);
	if (event.bus & WS9652A_USB_BUS_RESET) {
		ws9652a_reset(dev, true);
		return;
	}
	if (event.wrapper & WS9652A_USB_WRAPPER_DISCONNECT) {
		/* This reports controller disconnect, not a proven VBUS level.
		 * Cancel transfers and retain the pull-up for the next bus reset;
		 * do not manufacture an unpaired VBUS_REMOVED notification.
		 */
		ws9652a_reset(dev, false);
		return;
	}
	if (priv->awaiting_reset) {
		if (priv->reset_notify_pending) {
			ws9652a_notify_reset(dev);
		}
		/* Release the mutex before a queued RESET can run its stack teardown.
		 * Never deliver SETUP in the same iteration as the notification.
		 */
		return;
	}
	if ((event.bus & WS9652A_USB_BUS_SUSPEND) && !udc_is_suspended(dev)) {
		udc_set_suspended(dev, true);
		udc_submit_event(dev, UDC_EVT_SUSPEND, 0);
	}
	if ((event.bus & WS9652A_USB_BUS_RESUME) && udc_is_suspended(dev)) {
		udc_set_suspended(dev, false);
		udc_submit_event(dev, UDC_EVT_RESUME, 0);
	}

	if (event.ep0_csr & WS9652A_USB_EP0_SETUP_END) {
		/* A new SETUP aborts both old directions, even if its payload has
		 * not arrived yet. Never restart the obsolete IN request.
		 */
		if (!priv->out[0].active) {
			int ret = ws9652a_arm_rx(dev, &priv->out[0]);

			if (ret != 0) {
				udc_submit_event(dev, UDC_EVT_ERROR, ret);
				return;
			}
			rx_rearmed = true;
		}
		ws9652a_usb_hw_ep_disable(ws9652a_hw(dev), USB_CONTROL_EP_IN);
		ws9652a_cancel(dev, &priv->in[0], -ECONNRESET, false);
		/* Preserve the armed RX bounce buffer while cancelling requests. */
		ws9652a_cancel(dev, &priv->out[0], -ECONNRESET, true);
		priv->out[0].active = true;
		priv->phase = WS9652A_SETUP;
		priv->ep0_pending = false;
		ws9652a_usb_hw_setup_end_ack(ws9652a_hw(dev));
		new_setup = true;
	}
	if (event.ep0_csr & WS9652A_USB_EP0_SENT_STALL) {
		ws9652a_usb_hw_stall(ws9652a_hw(dev), USB_CONTROL_EP_OUT, false);
		priv->phase = WS9652A_SETUP;
		priv->in[0].cfg.stat.halted = false;
		priv->out[0].cfg.stat.halted = false;
	}
	/* The previous final IN and the next SETUP can be observed in one poll.
	 * Retire the acknowledged IN before classifying that received packet.
	 */
	if (!new_setup && (event.tx & BIT(0)) &&
	    !(event.ep0_csr & (WS9652A_USB_EP0_TX_READY | WS9652A_USB_EP0_DATA_END |
			      WS9652A_USB_EP0_SENT_STALL))) {
		ws9652a_tx_done(dev, &priv->in[0], 0);
	}
	if ((event.ep0_csr & WS9652A_USB_EP0_RX_READY) && !priv->ep0_pending && !rx_rearmed) {
		priv->out[0].packet_len = ws9652a_usb_hw_rx_count(ws9652a_hw(dev), 0);
		priv->out[0].active = false;
		priv->ep0_pending = true;
	}
	ws9652a_ep0_out(dev);
	for (unsigned int i = 1; i < WS9652A_USB_EP_COUNT; i++) {
		if (event.tx & BIT(i)) {
			ws9652a_tx_done(dev, &priv->in[i], (event.tx_error & BIT(i)) ? -EIO : 0);
		}
		if (event.rx & BIT(i)) {
			ws9652a_rx_done(dev, &priv->out[i], (event.rx_error & BIT(i)) ? -EIO : 0);
		}
	}

	for (unsigned int i = 0; i < WS9652A_USB_EP_COUNT; i++) {
		struct ws9652a_ep *out = &priv->out[i];

		ws9652a_start_tx(dev, &priv->in[i]);
		if (i != 0 && out->configured && !out->active && !out->cfg.stat.halted &&
		    udc_buf_peek(&out->cfg) != NULL) {
			int ret = ws9652a_arm_rx(dev, out);

			if (ret != 0) {
				ws9652a_complete(dev, out, ret);
			} else {
				udc_ep_set_busy(&out->cfg, true);
			}
		}
	}
}

static void ws9652a_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	struct ws9652a_udc_data *priv = udc_get_private(dev);

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	while (true) {
		k_sem_take(&priv->wake, K_MSEC(CONFIG_UDC_WS9652A_POLL_INTERVAL_MS));
		udc_lock_internal(dev, K_FOREVER);
		if (udc_is_enabled(dev)) {
			ws9652a_poll(dev);
		}
		udc_unlock_internal(dev);
	}
}

static int ws9652a_ep_enqueue(const struct device *dev, struct udc_ep_config *cfg,
			      struct net_buf *buf)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);

	udc_buf_put(cfg, buf);
	k_sem_give(&priv->wake);
	return 0;
}

static int ws9652a_ep_dequeue(const struct device *dev, struct udc_ep_config *cfg)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct ws9652a_ep *ep = ws9652a_ep(cfg);

	/* Abort the transfer without changing the host's endpoint data toggle. */
	if (priv->hw_initialized) {
		ws9652a_usb_hw_ep_cancel(ws9652a_hw(dev), cfg->addr);
	}
	ws9652a_cancel(dev, ep, -ECONNABORTED, false);
	if (USB_EP_GET_IDX(cfg->addr) == 0) {
		if (priv->phase == WS9652A_STATUS_IN && priv->ep0_pending) {
			/* Keep the consumed packet held until a replacement status,
			 * stall, or new SETUP. Reclassifying its RXREADY as SETUP would
			 * deliver the same request twice after status cancellation.
			 */
			return 0;
		}
		/* Removing a queued SETUP receiver must not stop an independent
		 * control IN already in progress on the other direction.
		 */
		if (cfg->addr == USB_CONTROL_EP_IN || priv->phase == WS9652A_DATA_OUT) {
			priv->phase = WS9652A_SETUP;
		}
		priv->ep0_pending = false;
		if (cfg->addr == USB_CONTROL_EP_OUT && udc_is_enabled(dev) &&
		    priv->hw_initialized && !priv->awaiting_reset) {
			return ws9652a_arm_rx(dev, ep);
		}
	}
	return 0;
}

static int ws9652a_ep_enable(const struct device *dev, struct udc_ep_config *cfg)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct ws9652a_ep *ep = ws9652a_ep(cfg);
	int ret;

	if (!priv->hw_initialized) {
		return -EAGAIN;
	}
	ret = ws9652a_usb_hw_ep_configure(ws9652a_hw(dev), cfg->addr, cfg->mps);

	ep->configured = ret == 0;
	ep->active = false;
	return ret;
}

static int ws9652a_ep_disable(const struct device *dev, struct udc_ep_config *cfg)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct ws9652a_ep *ep = ws9652a_ep(cfg);

	if (priv->hw_initialized) {
		ws9652a_usb_hw_ep_disable(ws9652a_hw(dev), cfg->addr);
	}
	ep->configured = false;
	ep->active = false;
	udc_ep_set_busy(cfg, false);
	return 0;
}

static int ws9652a_ep_set_halt(const struct device *dev, struct udc_ep_config *cfg)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct ws9652a_ep *ep = ws9652a_ep(cfg);

	if (USB_EP_GET_IDX(cfg->addr) == 0) {
		ws9652a_ep0_stall(dev);
	} else {
		if (priv->hw_initialized) {
			ws9652a_usb_hw_ep_disable(ws9652a_hw(dev), cfg->addr);
			ws9652a_usb_hw_stall(ws9652a_hw(dev), cfg->addr, true);
		}
		ep->active = false;
		udc_ep_set_busy(cfg, false);
		cfg->stat.halted = true;
	}
	return 0;
}

static int ws9652a_ep_clear_halt(const struct device *dev, struct udc_ep_config *cfg)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct ws9652a_ep *ep = ws9652a_ep(cfg);
	int ret;

	if (!priv->hw_initialized) {
		return -EAGAIN;
	}
	ret = ws9652a_usb_hw_ep_configure(ws9652a_hw(dev), cfg->addr, cfg->mps);

	if (ret != 0) {
		return ret;
	}
	ws9652a_usb_hw_stall(ws9652a_hw(dev), cfg->addr, false);
	ep->active = false;
	udc_ep_set_busy(cfg, false);
	cfg->stat.halted = false;
	k_sem_give(&priv->wake);
	return 0;
}

static int ws9652a_set_address(const struct device *dev, uint8_t address)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);

	if (!priv->hw_initialized) {
		return -EAGAIN;
	}
	/* addr_before_status=false: Zephyr calls this after status-IN completion. */
	ws9652a_usb_hw_set_address(ws9652a_hw(dev), address);
	return 0;
}

static int ws9652a_host_wakeup(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOTSUP;
}

static int ws9652a_enable(const struct device *dev)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	int ret;

	priv->reset_notify_pending = false;
	if (!priv->hw_initialized) {
		ws9652a_usb_hw_init(ws9652a_hw(dev));
		priv->hw_initialized = true;
	}
	priv->phase = WS9652A_SETUP;
	priv->ep0_pending = false;
	priv->awaiting_reset = true;
	ret = ws9652a_arm_rx(dev, &priv->out[0]);
	if (ret != 0) {
		ws9652a_usb_hw_disable(ws9652a_hw(dev));
		priv->hw_initialized = false;
		return ret;
	}
	ws9652a_usb_hw_connect(ws9652a_hw(dev), true);
	k_sem_give(&priv->wake);
	return 0;
}

static int ws9652a_disable(const struct device *dev)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct udc_data *data = dev->data;

	priv->reset_notify_pending = false;
	ws9652a_usb_hw_disable(ws9652a_hw(dev));
	priv->hw_initialized = false;
	priv->ep0_pending = false;
	priv->phase = WS9652A_SETUP;
	data->setup_pending = false;
	data->setup_valid = false;
	for (unsigned int i = 0; i < WS9652A_USB_EP_COUNT; i++) {
		ws9652a_cancel(dev, &priv->in[i], -ECONNABORTED, false);
		ws9652a_cancel(dev, &priv->out[i], -ECONNABORTED, false);
		priv->in[i].configured = i == 0;
		priv->out[i].configured = i == 0;
	}
	return 0;
}

static int ws9652a_init(const struct device *dev)
{
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	int ret;

	ws9652a_usb_hw_init(ws9652a_hw(dev));
	priv->hw_initialized = true;
	ret = udc_ep_enable_internal(dev, USB_CONTROL_EP_OUT, USB_EP_TYPE_CONTROL,
				     WS9652A_USB_MAX_PACKET, 0);
	if (ret != 0) {
		goto failed;
	}
	ret = udc_ep_enable_internal(dev, USB_CONTROL_EP_IN, USB_EP_TYPE_CONTROL,
				     WS9652A_USB_MAX_PACKET, 0);
	if (ret == 0) {
		return 0;
	}
	udc_ep_disable_internal(dev, USB_CONTROL_EP_OUT);

failed:
	ws9652a_usb_hw_disable(ws9652a_hw(dev));
	priv->hw_initialized = false;
	return ret;
}

static int ws9652a_shutdown(const struct device *dev)
{
	int ret;

	ws9652a_disable(dev);
	ret = udc_ep_disable_internal(dev, USB_CONTROL_EP_OUT);
	if (ret != 0) {
		return ret;
	}
	return udc_ep_disable_internal(dev, USB_CONTROL_EP_IN);
}

static void ws9652a_lock(const struct device *dev)
{
	udc_lock_internal(dev, K_FOREVER);
}

static void ws9652a_unlock(const struct device *dev)
{
	udc_unlock_internal(dev);
}

static const struct udc_api ws9652a_api = {
	.lock = ws9652a_lock,
	.unlock = ws9652a_unlock,
	.init = ws9652a_init,
	.enable = ws9652a_enable,
	.disable = ws9652a_disable,
	.shutdown = ws9652a_shutdown,
	.set_address = ws9652a_set_address,
	.host_wakeup = ws9652a_host_wakeup,
	.ep_enable = ws9652a_ep_enable,
	.ep_disable = ws9652a_ep_disable,
	.ep_enqueue = ws9652a_ep_enqueue,
	.ep_dequeue = ws9652a_ep_dequeue,
	.ep_set_halt = ws9652a_ep_set_halt,
	.ep_clear_halt = ws9652a_ep_clear_halt,
};

static int ws9652a_preinit(const struct device *dev)
{
	const struct ws9652a_udc_config *config = dev->config;
	struct ws9652a_udc_data *priv = udc_get_private(dev);
	struct udc_data *data = dev->data;
	int ret;

	k_mutex_init(&data->mutex);
	k_sem_init(&priv->wake, 0, 1);
	data->caps.mps0 = UDC_MPS0_64;
	/* TOP's EP0 DATAEND path consumes status OUT without a receive request. */
	data->caps.out_ack = true;
	for (unsigned int i = 0; i < WS9652A_USB_EP_COUNT; i++) {
		struct udc_ep_config *in = &priv->in[i].cfg;
		struct udc_ep_config *out = &priv->out[i].cfg;

		in->addr = USB_EP_DIR_IN | i;
		in->caps.in = true;
		in->caps.mps = WS9652A_USB_MAX_PACKET;
		in->caps.control = i == 0;
		in->caps.interrupt = i == 1;
		in->caps.bulk = i == 2;
		ret = udc_register_ep(dev, in);
		if (ret != 0) {
			return ret;
		}
		out->addr = USB_EP_DIR_OUT | i;
		if (i == 1) {
			/* EP1 is reserved for the CDC ACM interrupt-IN notification. */
			continue;
		}
		out->caps.out = true;
		out->caps.mps = WS9652A_USB_MAX_PACKET;
		out->caps.control = i == 0;
		out->caps.bulk = i == 2;
		ret = udc_register_ep(dev, out);
		if (ret != 0) {
			return ret;
		}
	}
	k_thread_create(&priv->thread, config->stack, config->stack_size, ws9652a_thread,
			(void *)dev, NULL, NULL, CONFIG_UDC_WS9652A_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&priv->thread, dev->name);
	return 0;
}

#define WS9652A_UDC_DEFINE(n)                                                                      \
	K_THREAD_STACK_DEFINE(ws9652a_stack_##n, CONFIG_UDC_WS9652A_STACK_SIZE);                   \
	static struct ws9652a_udc_data ws9652a_priv_##n;                                           \
	static struct udc_data ws9652a_data_##n = {                                                \
		.priv = &ws9652a_priv_##n,                                                         \
	};                                                                                         \
	static const struct ws9652a_udc_config ws9652a_config_##n = {                              \
		.hw =                                                                              \
			{                                                                          \
				.base = DT_INST_REG_ADDR_BY_NAME(n, usb),                          \
				.dma_base = DT_INST_REG_ADDR_BY_NAME(n, dma),                      \
				.dma_irq_base = DT_INST_REG_ADDR_BY_NAME(n, dma_irq),              \
				.clock_gate = DT_INST_REG_ADDR_BY_NAME(n, clock_gate),             \
			},                                                                         \
		.stack = ws9652a_stack_##n,                                                        \
		.stack_size = K_THREAD_STACK_SIZEOF(ws9652a_stack_##n),                            \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, ws9652a_preinit, NULL, &ws9652a_data_##n, &ws9652a_config_##n,    \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &ws9652a_api);

DT_INST_FOREACH_STATUS_OKAY(WS9652A_UDC_DEFINE)
