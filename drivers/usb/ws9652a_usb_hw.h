/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_WS9652A_USB_HW_H_
#define CWBEAT_WS9652A_USB_HW_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#define WS9652A_USB_EP_COUNT   3U
#define WS9652A_USB_MAX_PACKET 64U

#define WS9652A_USB_BUS_SUSPEND        BIT(0)
#define WS9652A_USB_BUS_RESUME         BIT(1)
#define WS9652A_USB_BUS_RESET          BIT(2)
#define WS9652A_USB_BUS_PHY            BIT(4)
#define WS9652A_USB_WRAPPER_DISCONNECT BIT(4)

#define WS9652A_USB_EP0_RX_READY   BIT(0)
#define WS9652A_USB_EP0_TX_READY   BIT(1)
#define WS9652A_USB_EP0_SENT_STALL BIT(2)
#define WS9652A_USB_EP0_DATA_END   BIT(3)
#define WS9652A_USB_EP0_SETUP_END  BIT(4)
#define WS9652A_USB_EP0_SEND_STALL BIT(5)
#define WS9652A_USB_EP0_RX_ACK     BIT(6)
#define WS9652A_USB_EP0_SETUP_ACK  BIT(7)

#define WS9652A_USB_TX_READY          BIT(0)
#define WS9652A_USB_TX_FIFO_NOT_EMPTY BIT(1)
#define WS9652A_USB_TX_UNDERRUN       BIT(2)
#define WS9652A_USB_TX_SENT_STALL     BIT(5)

/* All addresses are MMIO resources; packet storage belongs to the UDC. */
struct ws9652a_usb_hw {
	uintptr_t base;
	uintptr_t dma_base;
	uintptr_t dma_irq_base;
	uintptr_t clock_gate;
};

struct ws9652a_usb_event {
	uint32_t bus;
	uint32_t tx;
	uint32_t rx;
	uint32_t wrapper;
	uint32_t tx_error;
	uint32_t rx_error;
	uint32_t ep0_csr;
};

/* init leaves EP0 RX unarmed and disconnected; reset preserves connection. */
void ws9652a_usb_hw_init(const struct ws9652a_usb_hw *hw);
void ws9652a_usb_hw_reset(const struct ws9652a_usb_hw *hw);
void ws9652a_usb_hw_disable(const struct ws9652a_usb_hw *hw);
void ws9652a_usb_hw_connect(const struct ws9652a_usb_hw *hw, bool connected);
/* active_tx_mask uses endpoint-number bits for non-control IN requests. Poll
 * their CSR as well as IRQ status: a drained FIFO with TXREADY clear completes
 * an active request even when no TX interrupt was observed. EP0 is unchanged.
 */
void ws9652a_usb_hw_poll(const struct ws9652a_usb_hw *hw, struct ws9652a_usb_event *event,
			 uint8_t active_tx_mask);
void ws9652a_usb_hw_set_address(const struct ws9652a_usb_hw *hw, uint8_t address);
int ws9652a_usb_hw_ep_configure(const struct ws9652a_usb_hw *hw, uint8_t ep, uint16_t mps);
/* Cancelling a request preserves the endpoint data toggle and IRQ enable. */
void ws9652a_usb_hw_ep_cancel(const struct ws9652a_usb_hw *hw, uint8_t ep);
void ws9652a_usb_hw_ep_disable(const struct ws9652a_usb_hw *hw, uint8_t ep);
/* DMA buffers must be word aligned, remain live until cancellation/completion,
 * and accommodate size bytes. Supply a valid buffer even for a TX ZLP.
 * The backend never owns or releases buffers.
 */
int ws9652a_usb_hw_rx_arm(const struct ws9652a_usb_hw *hw, uint8_t ep, void *buffer, size_t size);
uint16_t ws9652a_usb_hw_rx_count(const struct ws9652a_usb_hw *hw, uint8_t ep);
void ws9652a_usb_hw_rx_ack(const struct ws9652a_usb_hw *hw, uint8_t ep, bool data_end);
int ws9652a_usb_hw_tx_start(const struct ws9652a_usb_hw *hw, uint8_t ep, const void *buffer,
			    size_t size, bool data_end);
void ws9652a_usb_hw_stall(const struct ws9652a_usb_hw *hw, uint8_t ep, bool stalled);
void ws9652a_usb_hw_setup_end_ack(const struct ws9652a_usb_hw *hw);

#endif
