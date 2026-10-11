/* SPDX-License-Identifier: Apache-2.0 */
#include "ws9652a_usb_hw.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>

/* Recovered from WS9652A ROM 0x1474..0x158c, 0x1888..0x1b0c,
 * 0x4b58..0x4c40 and 0x5108..0x5170. TOP 0x4a208/0x4a818 supplies
 * the controller reset sequence; 0x5a418 supplies endpoint setup.
 * This is a word-register derivative of the MUSB device core, with a
 * separate SoC DMA engine. No CPU-accessible FIFO port has been established.
 */
#define USB_FADDR       0x000U
#define USB_POWER       0x004U
#define USB_INTR_TX     0x008U
#define USB_INTR_RX     0x00cU
#define USB_INTR_TX_EN  0x010U
#define USB_INTR_RX_EN  0x014U
#define USB_INTR_BUS    0x018U
#define USB_INTR_BUS_EN 0x01cU
#define USB_INDEX       0x024U
#define USB_TX_MAXP     0x040U
#define USB_TX_CSR      0x044U
#define USB_RX_MAXP     0x048U
#define USB_RX_CSR      0x04cU
#define USB_RX_COUNT    0x050U
#define USB_RX_PACKETS  0x06cU
#define USB_TX_COUNT    0x070U
#define USB_DEVCTL      0x0a0U
#define USB_EP0_CSR     0x104U
#define USB_EP0_RXCOUNT 0x110U
#define USB_EP0_TXCOUNT 0x114U
#define USB_EP0_CONTROL 0x130U
#define USB_WRAP_STATUS 0x388U
#define USB_WRAP_MASK   0x390U
#define USB_WRAP_UNMASK 0x394U
#define USB_RUN         0x400U
#define USB_PHY_CONTROL 0x40cU
#define USB_RESET       0x414U

#define DMA_RX_CONTROL 0x0c0U
#define DMA_TX_CONTROL 0x120U
#define DMA_MASK0      0x004U
#define DMA_MASK1      0x008U
#define DMA_ACK0       0x00cU
#define DMA_ACK1       0x010U
#define DMA_USB_MASK   GENMASK(9, 4)
#define USB_EP_MASK    GENMASK(2, 0)
#define USB_SOFTCONN   BIT(6)
#define USB_HS_ENABLE  BIT(5)
#define USB_RUN_ENABLE BIT(13)
#define USB_DIR_IN     BIT(7)

static const uint16_t rx_address[] = {0x0c8, 0x0d8, 0x100};
static const uint16_t rx_length[] = {0x0d0, 0x0e0, 0x104};
static const uint16_t tx_address[] = {0x128, 0x138, 0x148};
static const uint16_t tx_length[] = {0x130, 0x140, 0x150};

static void update(uintptr_t address, uint32_t clear, uint32_t set)
{
	sys_write32((sys_read32(address) & ~clear) | set, address);
}

static void dma_cancel(const struct ws9652a_usb_hw *hw, uint8_t ep, bool in)
{
	uintptr_t control = hw->dma_base + (in ? DMA_TX_CONTROL : DMA_RX_CONTROL);

	/* Reset this channel before clearing its DMA enable. Command bits are
	 * self-clearing; never reset a channel belonging to another peripheral.
	 */
	update(control, 0, BIT(ep + 5U));
	update(control, BIT(ep + 10U), 0);
}

void ws9652a_usb_hw_connect(const struct ws9652a_usb_hw *hw, bool connected)
{
	update(hw->base + USB_POWER, USB_SOFTCONN, connected ? USB_SOFTCONN : 0);
}

void ws9652a_usb_hw_reset(const struct ws9652a_usb_hw *hw)
{
	for (uint8_t ep = 0; ep < WS9652A_USB_EP_COUNT; ep++) {
		dma_cancel(hw, ep, false);
		dma_cancel(hw, ep, true);
	}
	sys_write32(0, hw->base + USB_FADDR);
	sys_write32(BIT(0), hw->base + USB_INTR_TX_EN);
	sys_write32(0, hw->base + USB_INTR_RX_EN);
	sys_write32(WS9652A_USB_BUS_RESET | WS9652A_USB_BUS_RESUME | WS9652A_USB_BUS_SUSPEND,
		    hw->base + USB_INTR_BUS_EN);
}

void ws9652a_usb_hw_init(const struct ws9652a_usb_hw *hw)
{
	/* USB uses channels 4..9. Leave every unrelated DMA mask/ack untouched.
	 * Completion is taken from the USB core, so DMA IRQs remain masked.
	 */
	update(hw->dma_irq_base + DMA_MASK0, 0, DMA_USB_MASK);
	update(hw->dma_irq_base + DMA_MASK1, 0, DMA_USB_MASK);
	sys_write32(DMA_USB_MASK, hw->dma_irq_base + DMA_ACK0);
	sys_write32(DMA_USB_MASK, hw->dma_irq_base + DMA_ACK1);
	ws9652a_usb_hw_connect(hw, false);
	update(hw->clock_gate, BIT(5), 0);
	sys_write32(0, hw->base + USB_RESET);
	k_busy_wait(100);
	sys_write32(1, hw->base + USB_RESET);
	k_busy_wait(100);
	update(hw->base + USB_PHY_CONTROL, 0, BIT(0) | BIT(4));
	update(hw->base + USB_RUN, USB_RUN_ENABLE, 0);
	sys_write32(0x3f, hw->base + USB_WRAP_MASK);
	sys_write32(0x3f, hw->base + USB_WRAP_STATUS);
	update(hw->base + USB_POWER, USB_SOFTCONN | USB_HS_ENABLE, 0);
	ws9652a_usb_hw_reset(hw);
	/* The polling worker owns the wrapper events; no NVIC interrupt needed. */
	sys_write32(BIT(0) | BIT(4), hw->base + USB_WRAP_UNMASK);
	update(hw->base + USB_RUN, 0, USB_RUN_ENABLE);
	/* Native TOP 0x4a7c4 sets this EP0 control word to 2. It does not
	 * remap DMA: the native endpoint-to-DMA table is the identity mapping.
	 */
	sys_write32(2, hw->base + USB_EP0_CONTROL);
	update(hw->base + USB_DEVCTL, 0, BIT(0));
}

void ws9652a_usb_hw_disable(const struct ws9652a_usb_hw *hw)
{
	ws9652a_usb_hw_connect(hw, false);
	ws9652a_usb_hw_reset(hw);
	sys_write32(0, hw->base + USB_INTR_TX_EN);
	sys_write32(0, hw->base + USB_INTR_RX_EN);
	sys_write32(0, hw->base + USB_INTR_BUS_EN);
	sys_write32(0x3f, hw->base + USB_WRAP_MASK);
	update(hw->base + USB_RUN, USB_RUN_ENABLE, 0);
}

void ws9652a_usb_hw_poll(const struct ws9652a_usb_hw *hw, struct ws9652a_usb_event *event,
			 uint8_t active_tx_mask)
{
	event->wrapper = sys_read32(hw->base + USB_WRAP_STATUS) & 0x3fU;
	if (event->wrapper != 0U) {
		sys_write32(event->wrapper, hw->base + USB_WRAP_STATUS);
	}
	/* These core interrupt registers are read-to-clear. Snapshot each once. */
	event->bus = sys_read32(hw->base + USB_INTR_BUS);
	event->tx = sys_read32(hw->base + USB_INTR_TX) & USB_EP_MASK;
	event->rx = sys_read32(hw->base + USB_INTR_RX) & (USB_EP_MASK & ~BIT(0));
	event->ep0_csr = sys_read32(hw->base + USB_EP0_CSR);
	event->tx_error = 0;
	event->rx_error = 0;
	for (uint8_t ep = 1; ep < WS9652A_USB_EP_COUNT; ep++) {
		if (((event->tx | active_tx_mask) & BIT(ep)) != 0U) {
			sys_write32(ep, hw->base + USB_INDEX);
			uint32_t csr = sys_read32(hw->base + USB_TX_CSR);

			if ((csr & WS9652A_USB_TX_SENT_STALL) != 0U) {
				event->tx |= BIT(ep);
				event->tx_error |= BIT(ep);
			} else if ((csr & (WS9652A_USB_TX_READY | WS9652A_USB_TX_FIFO_NOT_EMPTY)) !=
				   0U) {
				/* TXREADY may clear while the FIFO is still draining. */
				event->tx &= ~BIT(ep);
			} else if ((active_tx_mask & BIT(ep)) != 0U) {
				/* The initial CDC ZLP can leave a drained endpoint without
				 * an observed TX IRQ. Poll completion only for a request
				 * already armed by the owner, after its TXREADY write.
				 */
				event->tx |= BIT(ep);
			}
			/* UNDERRUN means the endpoint NAKed an IN token while empty.
			 * Native TOP 0x43900..0x4390e clears it and continues; it must
			 * not fail or halt the request that follows that idle period.
			 */
			if ((csr & (WS9652A_USB_TX_UNDERRUN | WS9652A_USB_TX_SENT_STALL)) != 0U) {
				sys_write32(csr & ~(WS9652A_USB_TX_UNDERRUN | WS9652A_USB_TX_SENT_STALL),
					    hw->base + USB_TX_CSR);
			}
		}
		if ((event->rx & BIT(ep)) != 0U) {
			sys_write32(ep, hw->base + USB_INDEX);
			uint32_t csr = sys_read32(hw->base + USB_RX_CSR);

			if ((csr & (BIT(2) | BIT(3) | BIT(6))) != 0U) {
				event->rx_error |= BIT(ep);
			} else if ((csr & BIT(0)) == 0U) {
				event->rx &= ~BIT(ep);
			}
		}
	}
}

void ws9652a_usb_hw_set_address(const struct ws9652a_usb_hw *hw, uint8_t address)
{
	sys_write32(address & 0x7fU, hw->base + USB_FADDR);
}

int ws9652a_usb_hw_ep_configure(const struct ws9652a_usb_hw *hw, uint8_t ep, uint16_t mps)
{
	bool in = (ep & USB_DIR_IN) != 0U;
	uint8_t index = ep & ~USB_DIR_IN;

	if (index >= WS9652A_USB_EP_COUNT || mps == 0 || mps > WS9652A_USB_MAX_PACKET) {
		return -EINVAL;
	}
	if (index == 0) {
		return mps == WS9652A_USB_MAX_PACKET ? 0 : -EINVAL;
	}
	ws9652a_usb_hw_ep_cancel(hw, ep);
	sys_write32(index, hw->base + USB_INDEX);
	sys_write32(mps, hw->base + (in ? USB_TX_MAXP : USB_RX_MAXP));
	/* Bulk and interrupt share the non-isochronous CSR setting. */
	sys_write32(in ? BIT(6) : BIT(7), hw->base + (in ? USB_TX_CSR : USB_RX_CSR));
	if (in) {
		update(hw->base + USB_INTR_TX_EN, 0, BIT(index));
	}
	return 0;
}

void ws9652a_usb_hw_ep_cancel(const struct ws9652a_usb_hw *hw, uint8_t ep)
{
	bool in = (ep & USB_DIR_IN) != 0U;
	uint8_t index = ep & ~USB_DIR_IN;

	if (index >= WS9652A_USB_EP_COUNT) {
		return;
	}
	dma_cancel(hw, index, in);
	if (index == 0) {
		/* EP0 shares its FIFO between directions. A new SETUP can already
		 * be present while the preceding IN request is being cancelled.
		 * Follow native TOP's DMA-only cancellation; SETUPEND is serviced
		 * separately by the control state machine.
		 */
		return;
	}
	sys_write32(index, hw->base + USB_INDEX);
	/* Flush uses the compatible MUSB CSR bits (Linux musb_gadget.c nuke),
	 * beyond the stock TOP DMA-only cancellation. TXPKTRDY must be clear;
	 * write twice to discard both FIFO banks without resetting DATA0/1.
	 */
	uintptr_t csr = hw->base + (in ? USB_TX_CSR : USB_RX_CSR);
	uint32_t flush = in ? BIT(3) : BIT(4);

	sys_write32(flush, csr);
	sys_write32(flush, csr);
}

void ws9652a_usb_hw_ep_disable(const struct ws9652a_usb_hw *hw, uint8_t ep)
{
	bool in = (ep & USB_DIR_IN) != 0U;
	uint8_t index = ep & ~USB_DIR_IN;

	if (index >= WS9652A_USB_EP_COUNT) {
		return;
	}
	ws9652a_usb_hw_ep_cancel(hw, ep);
	if (index != 0) {
		update(hw->base + (in ? USB_INTR_TX_EN : USB_INTR_RX_EN), BIT(index), 0);
	}
}

static bool valid_buffer(uint8_t ep, const void *buffer, size_t size)
{
	return ep < WS9652A_USB_EP_COUNT && size <= WS9652A_USB_MAX_PACKET && buffer != NULL &&
	       (((uintptr_t)buffer & 3U) == 0U);
}

int ws9652a_usb_hw_rx_arm(const struct ws9652a_usb_hw *hw, uint8_t ep, void *buffer, size_t size)
{
	if (!valid_buffer(ep, buffer, size) || size == 0) {
		return -EINVAL;
	}
	dma_cancel(hw, ep, false);
	update(hw->dma_base + DMA_RX_CONTROL, 0, BIT(ep + 10U));
	sys_write32((uint32_t)(uintptr_t)buffer & ~BIT(31), hw->dma_base + rx_address[ep]);
	sys_write32(size, hw->dma_base + rx_length[ep]);
	barrier_dmem_fence_full();
	update(hw->dma_base + DMA_RX_CONTROL, 0, BIT(ep));
	if (ep != 0) {
		sys_write32(ep, hw->base + USB_INDEX);
		/* ROM 0x4da0 programs ceil(length / 512) packets. */
		sys_write32(1, hw->base + USB_RX_PACKETS);
		update(hw->base + USB_INTR_RX_EN, 0, BIT(ep));
	}
	return 0;
}

uint16_t ws9652a_usb_hw_rx_count(const struct ws9652a_usb_hw *hw, uint8_t ep)
{
	if (ep >= WS9652A_USB_EP_COUNT) {
		return 0;
	}
	barrier_dmem_fence_full();
	/* RXREADY describes a packet in the USB core. Native TOP reads its
	 * RXCOUNT here (EP0: 0x52944, other endpoints: 0x4398a), not the DMA
	 * completion count, which need not be valid at this boundary.
	 */
	if (ep == 0) {
		return sys_read32(hw->base + USB_EP0_RXCOUNT);
	}
	sys_write32(ep, hw->base + USB_INDEX);
	return sys_read32(hw->base + USB_RX_COUNT);
}

void ws9652a_usb_hw_rx_ack(const struct ws9652a_usb_hw *hw, uint8_t ep, bool data_end)
{
	if (ep == 0) {
		sys_write32(WS9652A_USB_EP0_RX_ACK | (data_end ? WS9652A_USB_EP0_DATA_END : 0),
			    hw->base + USB_EP0_CSR);
	} else if (ep < WS9652A_USB_EP_COUNT) {
		sys_write32(ep, hw->base + USB_INDEX);
		update(hw->base + USB_RX_CSR, BIT(0) | BIT(2) | BIT(3) | BIT(6), 0);
	}
}

int ws9652a_usb_hw_tx_start(const struct ws9652a_usb_hw *hw, uint8_t ep, const void *buffer,
			    size_t size, bool data_end)
{
	if (!valid_buffer(ep, buffer, size)) {
		return -EINVAL;
	}
	dma_cancel(hw, ep, true);
	/* TOP 0x5a61e also programs and triggers zero-length DMA for a ZLP,
	 * using the endpoint's valid bounce buffer. Preserve that sequence.
	 */
	update(hw->dma_base + DMA_TX_CONTROL, 0, BIT(ep + 10U));
	sys_write32((uint32_t)(uintptr_t)buffer & ~BIT(31), hw->dma_base + tx_address[ep]);
	sys_write32(size, hw->dma_base + tx_length[ep]);
	barrier_dmem_fence_full();
	update(hw->dma_base + DMA_TX_CONTROL, 0, BIT(ep));
	if (ep == 0) {
		sys_write32(size, hw->base + USB_EP0_TXCOUNT);
		sys_write32(WS9652A_USB_EP0_TX_READY | (data_end ? WS9652A_USB_EP0_DATA_END : 0),
			    hw->base + USB_EP0_CSR);
	} else {
		sys_write32(ep, hw->base + USB_INDEX);
		sys_write32(size, hw->base + USB_TX_COUNT);
		update(hw->base + USB_TX_CSR, 0, BIT(0));
	}
	return 0;
}

void ws9652a_usb_hw_stall(const struct ws9652a_usb_hw *hw, uint8_t ep, bool stalled)
{
	bool in = (ep & USB_DIR_IN) != 0U;
	uint8_t index = ep & ~USB_DIR_IN;

	if (index >= WS9652A_USB_EP_COUNT) {
		return;
	}
	if (index == 0) {
		sys_write32(stalled ? WS9652A_USB_EP0_SEND_STALL | WS9652A_USB_EP0_RX_ACK : 0,
			    hw->base + USB_EP0_CSR);
		return;
	}
	sys_write32(index, hw->base + USB_INDEX);
	uintptr_t csr = hw->base + (in ? USB_TX_CSR : USB_RX_CSR);
	uint32_t send_stall = in ? BIT(4) : BIT(5);
	uint32_t sent_stall = in ? BIT(5) : BIT(6);
	uint32_t clear_toggle = in ? BIT(6) : BIT(7);

	update(csr, send_stall | sent_stall, stalled ? send_stall : clear_toggle);
}

void ws9652a_usb_hw_setup_end_ack(const struct ws9652a_usb_hw *hw)
{
	sys_write32(WS9652A_USB_EP0_SETUP_ACK, hw->base + USB_EP0_CSR);
}
