/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define CORE    0x40048000U
#define DMA     0x4002e000U
#define DMA_IRQ 0x4002f000U
#define CLOCK   0x4002f900U

static uint32_t core[0x420 / 4], indexed[3][0x80 / 4], dma[0x160 / 4];
static uint32_t dma_irq[0x20 / 4], clock_gate;
static unsigned int reads[ARRAY_SIZE(core)];
static struct {
	uintptr_t address;
	uint32_t value;
} writes[256];
static unsigned int nwrites, delays;
static uint8_t packet[64] __aligned(4);

static uint32_t *reg_at(uintptr_t address)
{
	if (address >= CORE && address < CORE + sizeof(core)) {
		uint32_t off = address - CORE;

		if (off >= 0x40 && off <= 0x70) {
			zassert_true(core[0x24 / 4] < 3);
			return &indexed[core[0x24 / 4]][off / 4];
		}
		return &core[off / 4];
	}
	if (address >= DMA && address < DMA + sizeof(dma)) {
		return &dma[(address - DMA) / 4];
	}
	if (address >= DMA_IRQ && address < DMA_IRQ + sizeof(dma_irq)) {
		return &dma_irq[(address - DMA_IRQ) / 4];
	}
	zassert_equal(address, CLOCK, "Unexpected access outside USB-owned MMIO");
	return &clock_gate;
}

static uint32_t mock_read(uintptr_t address)
{
	uint32_t *reg = reg_at(address);
	uint32_t value = *reg;

	if (address >= CORE && address < CORE + sizeof(core)) {
		reads[(address - CORE) / 4]++;
	}
	if (address == CORE + 8 || address == CORE + 0xc || address == CORE + 0x18) {
		*reg = 0;
	}
	return value;
}

static void mock_write(uint32_t value, uintptr_t address)
{
	zassert_true(nwrites < ARRAY_SIZE(writes));
	writes[nwrites].address = address;
	writes[nwrites++].value = value;
	uint32_t *reg = reg_at(address);

	if (address == CORE + 0x388) {
		*reg &= ~value;
	} else if (address == DMA + 0xc0 || address == DMA + 0x120) {
		/* Channel trigger/reset command bits clear themselves. */
		*reg = value & ~(GENMASK(2, 0) | GENMASK(7, 5));
	} else {
		*reg = value;
	}
}

static void mock_delay(uint32_t usec)
{
	zassert_equal(usec, 100);
	delays++;
}

#define sys_read32  mock_read
#define sys_write32 mock_write
#define k_busy_wait mock_delay
#include "../../../../drivers/usb/ws9652a_usb_hw.c"

static const struct ws9652a_usb_hw hw = {
	.base = CORE,
	.dma_base = DMA,
	.dma_irq_base = DMA_IRQ,
	.clock_gate = CLOCK,
};

static bool saw_write(uintptr_t address, uint32_t value)
{
	for (unsigned int i = 0; i < nwrites; i++) {
		if (writes[i].address == address && writes[i].value == value) {
			return true;
		}
	}
	return false;
}

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	memset(core, 0, sizeof(core));
	memset(indexed, 0, sizeof(indexed));
	memset(dma, 0, sizeof(dma));
	memset(dma_irq, 0, sizeof(dma_irq));
	memset(reads, 0, sizeof(reads));
	nwrites = 0;
	delays = 0;
	clock_gate = UINT32_MAX;
}

ZTEST(ws9652a_usb_hw, test_init_preserves_unrelated_dma_and_does_not_arm_buffers)
{
	dma_irq[1] = BIT(1) | BIT(22);
	dma_irq[2] = BIT(3) | BIT(27);
	dma[0xc0 / 4] = BIT(20);
	dma[0x120 / 4] = BIT(25);
	core[4 / 4] = BIT(5) | BIT(6) | BIT(7);
	ws9652a_usb_hw_init(&hw);
	zassert_equal(dma_irq[1], BIT(1) | BIT(22) | GENMASK(9, 4));
	zassert_equal(dma_irq[2], BIT(3) | BIT(27) | GENMASK(9, 4));
	zassert_equal(dma_irq[3], GENMASK(9, 4));
	zassert_equal(dma_irq[4], GENMASK(9, 4));
	zassert_equal(dma[0xc0 / 4], BIT(20));
	zassert_equal(dma[0x120 / 4], BIT(25));
	zassert_equal(clock_gate, UINT32_MAX & ~BIT(5));
	zassert_equal(core[4 / 4], BIT(7));
	zassert_equal(core[0x40c / 4], BIT(0) | BIT(4));
	zassert_equal(core[0x400 / 4], BIT(13));
	zassert_equal(delays, 2);
	zassert_equal(core[0x130 / 4], 2);
	zassert_equal(core[0xa0 / 4] & BIT(0), BIT(0));
	for (unsigned int i = 0; i < nwrites; i++) {
		if (writes[i].address == DMA + 0xc0 || writes[i].address == DMA + 0x120) {
			zassert_equal(writes[i].value & GENMASK(2, 0), 0);
		}
	}
}

ZTEST(ws9652a_usb_hw, test_rx_dma_uses_recovered_distinct_registers)
{
	static const uintptr_t address[] = {DMA + 0xc8, DMA + 0xd8, DMA + 0x100};
	static const uintptr_t length[] = {DMA + 0xd0, DMA + 0xe0, DMA + 0x104};

	for (unsigned int ep = 0; ep < 3; ep++) {
		zassert_ok(ws9652a_usb_hw_rx_arm(&hw, ep, packet, sizeof(packet)));
		zassert_true(saw_write(address[ep], (uintptr_t)packet & ~BIT(31)));
		zassert_true(saw_write(length[ep], 64));
		zassert_true(dma[0xc0 / 4] & BIT(ep + 10));
	}
	zassert_equal(core[0x14 / 4], BIT(1) | BIT(2));
}

ZTEST(ws9652a_usb_hw, test_tx_dma_and_packet_ready_cover_all_three_endpoints)
{
	static const uintptr_t address[] = {DMA + 0x128, DMA + 0x138, DMA + 0x148};
	static const uintptr_t length[] = {DMA + 0x130, DMA + 0x140, DMA + 0x150};

	for (unsigned int ep = 0; ep < 3; ep++) {
		zassert_ok(ws9652a_usb_hw_tx_start(&hw, ep, packet, 13, true));
		zassert_true(saw_write(address[ep], (uintptr_t)packet & ~BIT(31)));
		zassert_true(saw_write(length[ep], 13));
	}
	zassert_equal(core[0x114 / 4], 13);
	zassert_equal(core[0x104 / 4], BIT(1) | BIT(3));
	zassert_equal(indexed[1][0x70 / 4], 13);
	zassert_equal(indexed[2][0x70 / 4], 13);
	zassert_true(indexed[1][0x44 / 4] & BIT(0));
	zassert_true(indexed[2][0x44 / 4] & BIT(0));
}

ZTEST(ws9652a_usb_hw, test_zlp_arms_zero_length_dma_with_owned_buffer)
{
	zassert_ok(ws9652a_usb_hw_tx_start(&hw, 0, packet, 0, true));
	zassert_equal(core[0x114 / 4], 0);
	zassert_equal(core[0x104 / 4], BIT(1) | BIT(3));
	zassert_equal(dma[0x128 / 4], (uintptr_t)packet & ~BIT(31));
	zassert_equal(dma[0x130 / 4], 0);
	zassert_equal(dma[0x120 / 4], BIT(10));
	zassert_true(saw_write(DMA + 0x120, BIT(10) | BIT(0)));
}

ZTEST(ws9652a_usb_hw, test_invalid_buffers_and_endpoint_config_do_not_touch_hardware)
{
	zassert_equal(ws9652a_usb_hw_rx_arm(&hw, 3, packet, 64), -EINVAL);
	zassert_equal(ws9652a_usb_hw_rx_arm(&hw, 0, packet + 1, 63), -EINVAL);
	zassert_equal(ws9652a_usb_hw_rx_arm(&hw, 0, NULL, 0), -EINVAL);
	zassert_equal(ws9652a_usb_hw_tx_start(&hw, 2, packet, 65, false), -EINVAL);
	zassert_equal(ws9652a_usb_hw_tx_start(&hw, 2, NULL, 1, false), -EINVAL);
	zassert_equal(ws9652a_usb_hw_tx_start(&hw, 2, NULL, 0, false), -EINVAL);
	zassert_equal(ws9652a_usb_hw_ep_configure(&hw, 0x83, 64), -EINVAL);
	zassert_equal(ws9652a_usb_hw_ep_configure(&hw, 0x80, 8), -EINVAL);
	zassert_equal(nwrites, 0);
}

ZTEST(ws9652a_usb_hw, test_dma_status_alone_does_not_complete_idle_endpoints)
{
	struct ws9652a_usb_event event;

	/* DMA completes when bytes reach the controller, before host ACK. */
	dma_irq[0x14 / 4] = GENMASK(9, 4);
	dma_irq[0x18 / 4] = GENMASK(9, 4);
	ws9652a_usb_hw_poll(&hw, &event, 0);
	zassert_equal(event.tx, 0);
	core[0x08 / 4] = BIT(2) | BIT(7);
	core[0x0c / 4] = BIT(1);
	indexed[1][0x4c / 4] = BIT(0);
	core[0x18 / 4] = BIT(2);
	core[0x104 / 4] = BIT(0) | BIT(4);
	ws9652a_usb_hw_poll(&hw, &event, 0);
	zassert_equal(event.tx, BIT(2));
	zassert_equal(event.rx, BIT(1));
	zassert_equal(event.bus, BIT(2));
	zassert_equal(event.ep0_csr, BIT(0) | BIT(4));
	zassert_equal(reads[8 / 4], 2);
	zassert_equal(reads[0xc / 4], 2);
	zassert_equal(reads[0x18 / 4], 2);
	ws9652a_usb_hw_poll(&hw, &event, 0);
	zassert_equal(event.tx | event.rx | event.bus, 0);
}

ZTEST(ws9652a_usb_hw, test_errors_and_busy_packets_cannot_report_success)
{
	struct ws9652a_usb_event event;

	core[0x08 / 4] = BIT(1) | BIT(2);
	indexed[1][0x44 / 4] = BIT(0);
	indexed[2][0x44 / 4] = BIT(5);
	core[0x0c / 4] = BIT(1) | BIT(2);
	indexed[1][0x4c / 4] = 0;
	indexed[2][0x4c / 4] = BIT(6);
	ws9652a_usb_hw_poll(&hw, &event, 0);
	zassert_equal(event.tx, BIT(2));
	zassert_equal(event.tx_error, BIT(2));
	zassert_equal(event.rx, BIT(2));
	zassert_equal(event.rx_error, BIT(2));
}

ZTEST(ws9652a_usb_hw, test_tx_underrun_is_nonfatal_but_ready_packet_waits_for_ack)
{
	struct ws9652a_usb_event event;

	core[0x08 / 4] = BIT(2);
	indexed[2][0x44 / 4] = BIT(2) | BIT(0);
	ws9652a_usb_hw_poll(&hw, &event, 0);
	zassert_equal(event.tx_error, 0);
	zassert_equal(event.tx, 0, "UNDERRUN does not prove host acknowledgment");
	zassert_equal(indexed[2][0x44 / 4], BIT(0));
	core[0x08 / 4] = BIT(2);
	indexed[2][0x44 / 4] = BIT(2);
	ws9652a_usb_hw_poll(&hw, &event, 0);
	zassert_equal(event.tx_error, 0);
	zassert_equal(event.tx, BIT(2));
	zassert_equal(indexed[2][0x44 / 4], 0);
}

ZTEST(ws9652a_usb_hw, test_active_zlp_completes_without_interrupt_after_fifo_drains)
{
	struct ws9652a_usb_event event;

	zassert_ok(ws9652a_usb_hw_tx_start(&hw, 2, packet, 0, false));
	zassert_equal(dma[0x148 / 4], (uintptr_t)packet & ~BIT(31));
	zassert_equal(dma[0x150 / 4], 0);
	ws9652a_usb_hw_poll(&hw, &event, BIT(2));
	zassert_equal(event.tx, 0, "TXREADY still describes an unconsumed ZLP");

	/* Captured before DTR: TXREADY cleared, but the ZLP is still in the FIFO. */
	indexed[2][0x44 / 4] = BIT(1);
	dma_irq[0x18 / 4] = BIT(8);
	ws9652a_usb_hw_poll(&hw, &event, BIT(2));
	zassert_equal(event.tx, 0);
	zassert_equal(event.tx_error, 0);
	zassert_equal(indexed[2][0x44 / 4], BIT(1));

	/* Captured after DTR: FIFO drained, UNDERRUN set, no TX interrupt. */
	indexed[2][0x44 / 4] = BIT(2);
	ws9652a_usb_hw_poll(&hw, &event, BIT(2));
	zassert_equal(event.tx, BIT(2));
	zassert_equal(event.tx_error, 0);
	zassert_equal(indexed[2][0x44 / 4], 0);
	ws9652a_usb_hw_poll(&hw, &event, 0);
	zassert_equal(event.tx, 0, "An inactive empty endpoint cannot complete again");
}

ZTEST(ws9652a_usb_hw, test_tx_interrupt_with_fifo_data_waits_for_drain)
{
	struct ws9652a_usb_event event;

	zassert_ok(ws9652a_usb_hw_tx_start(&hw, 1, packet, 13, false));
	core[0x08 / 4] = BIT(1);
	indexed[1][0x44 / 4] = BIT(1);
	ws9652a_usb_hw_poll(&hw, &event, BIT(1));
	zassert_equal(event.tx, 0, "An interrupt must not retire bytes still in the FIFO");
	zassert_equal(event.tx_error, 0);
	zassert_equal(core[0x08 / 4], 0);
	indexed[1][0x44 / 4] = 0;
	ws9652a_usb_hw_poll(&hw, &event, BIT(1));
	zassert_equal(event.tx, BIT(1));
	zassert_equal(event.tx_error, 0);
}

ZTEST(ws9652a_usb_hw, test_active_tx_ready_waits_even_with_empty_fifo_and_underrun)
{
	struct ws9652a_usb_event event;

	indexed[2][0x44 / 4] = BIT(0) | BIT(2);
	ws9652a_usb_hw_poll(&hw, &event, BIT(2));
	zassert_equal(event.tx, 0);
	zassert_equal(event.tx_error, 0);
	zassert_equal(indexed[2][0x44 / 4], BIT(0));
	core[0x08 / 4] = BIT(2);
	ws9652a_usb_hw_poll(&hw, &event, BIT(2));
	zassert_equal(event.tx, 0);
}

ZTEST(ws9652a_usb_hw, test_active_stalled_tx_reports_error_without_interrupt)
{
	struct ws9652a_usb_event event;

	indexed[2][0x44 / 4] = BIT(5) | BIT(1) | BIT(0);
	ws9652a_usb_hw_poll(&hw, &event, BIT(2));
	zassert_equal(event.tx, BIT(2));
	zassert_equal(event.tx_error, BIT(2));
	zassert_equal(indexed[2][0x44 / 4], BIT(1) | BIT(0));
}

ZTEST(ws9652a_usb_hw, test_idle_and_control_endpoints_require_controller_interrupt)
{
	struct ws9652a_usb_event event;

	/* Even an accidental control bit in the mask cannot synthesize EP0 TX. */
	ws9652a_usb_hw_poll(&hw, &event, BIT(0));
	zassert_equal(event.tx, 0);
	zassert_equal(reads[0x44 / 4], 0);
	core[0x08 / 4] = BIT(0);
	core[0x104 / 4] = WS9652A_USB_EP0_DATA_END;
	ws9652a_usb_hw_poll(&hw, &event, 0);
	zassert_equal(event.tx, BIT(0));
	zassert_equal(event.ep0_csr, WS9652A_USB_EP0_DATA_END);
	zassert_equal(event.tx_error, 0);
}

ZTEST(ws9652a_usb_hw, test_receive_counts_use_usb_rxready_not_dma_completion)
{
	/* A packet is ready before a DMA completion count is available. The
	 * original TOP reads USB RXCOUNT at this boundary, including SETUP.
	 */
	core[0x104 / 4] = BIT(0);
	core[0x110 / 4] = 8;
	indexed[1][0x50 / 4] = 17;
	indexed[2][0x50 / 4] = 63;
	core[0x24 / 4] = 2;
	zassert_equal(ws9652a_usb_hw_rx_count(&hw, 0), 8);
	zassert_equal(core[0x24 / 4], 2, "EP0 RXCOUNT is not indexed");
	zassert_equal(ws9652a_usb_hw_rx_count(&hw, 1), 17);
	zassert_equal(core[0x24 / 4], 1);
	zassert_equal(ws9652a_usb_hw_rx_count(&hw, 2), 63);
	zassert_equal(core[0x24 / 4], 2);
	zassert_equal(dma[0x108 / 4] | dma[0x10c / 4] | dma[0x110 / 4], 0);
}

ZTEST(ws9652a_usb_hw, test_ep0_commands_use_separate_setup_end_and_data_end_bits)
{
	ws9652a_usb_hw_setup_end_ack(&hw);
	zassert_equal(core[0x104 / 4], BIT(7));
	ws9652a_usb_hw_rx_ack(&hw, 0, false);
	zassert_equal(core[0x104 / 4], BIT(6));
	ws9652a_usb_hw_rx_ack(&hw, 0, true);
	zassert_equal(core[0x104 / 4], BIT(6) | BIT(3));
	ws9652a_usb_hw_stall(&hw, 0x80, true);
	zassert_equal(core[0x104 / 4], BIT(6) | BIT(5));
	ws9652a_usb_hw_set_address(&hw, 23);
	zassert_equal(core[0], 23);
}

ZTEST(ws9652a_usb_hw, test_endpoint_configuration_and_stall_clear_reset_toggles)
{
	zassert_ok(ws9652a_usb_hw_ep_configure(&hw, 0x81, 16));
	zassert_equal(indexed[1][0x40 / 4], 16);
	zassert_equal(indexed[1][0x44 / 4], BIT(6));
	zassert_ok(ws9652a_usb_hw_ep_configure(&hw, 0x02, 64));
	zassert_equal(indexed[2][0x48 / 4], 64);
	zassert_equal(indexed[2][0x4c / 4], BIT(7));
	ws9652a_usb_hw_stall(&hw, 0x81, true);
	zassert_true(indexed[1][0x44 / 4] & BIT(4));
	ws9652a_usb_hw_stall(&hw, 0x81, false);
	zassert_equal(indexed[1][0x44 / 4], BIT(6));
	ws9652a_usb_hw_stall(&hw, 0x02, true);
	zassert_true(indexed[2][0x4c / 4] & BIT(5));
	ws9652a_usb_hw_stall(&hw, 0x02, false);
	zassert_equal(indexed[2][0x4c / 4], BIT(7));
}

ZTEST(ws9652a_usb_hw, test_ep0_cancel_preserves_new_setup_in_shared_fifo)
{
	core[0x104 / 4] = BIT(0) | BIT(4);
	ws9652a_usb_hw_ep_cancel(&hw, 0x80);
	zassert_equal(core[0x104 / 4], BIT(0) | BIT(4));
	for (unsigned int i = 0; i < nwrites; i++) {
		zassert_not_equal(writes[i].address, CORE + 0x104);
	}
}

ZTEST(ws9652a_usb_hw, test_cancel_flushes_pending_packet_without_changing_toggle_or_irq)
{
	core[0x10 / 4] = BIT(1);
	indexed[1][0x44 / 4] = BIT(0) | BIT(1);
	dma[0x120 / 4] = BIT(11);
	ws9652a_usb_hw_ep_cancel(&hw, 0x81);
	zassert_equal(core[0x10 / 4], BIT(1));
	zassert_equal(dma[0x120 / 4] & BIT(11), 0);
	unsigned int flushes = 0;

	for (unsigned int i = 0; i < nwrites; i++) {
		if (writes[i].address == CORE + 0x44) {
			zassert_equal(writes[i].value, BIT(3));
			flushes++;
		}
	}
	zassert_equal(flushes, 2);
}

ZTEST(ws9652a_usb_hw, test_reset_and_disable_cancel_dma_before_buffer_release)
{
	core[4 / 4] = BIT(6);
	core[0] = 17;
	dma[0xc0 / 4] = GENMASK(12, 10) | BIT(24);
	dma[0x120 / 4] = GENMASK(12, 10) | BIT(25);
	ws9652a_usb_hw_reset(&hw);
	zassert_equal(dma[0xc0 / 4], BIT(24));
	zassert_equal(dma[0x120 / 4], BIT(25));
	zassert_equal(core[4 / 4], BIT(6));
	zassert_equal(core[0], 0);
	zassert_equal(core[0x10 / 4], BIT(0));
	zassert_equal(core[0x14 / 4], 0);
	ws9652a_usb_hw_disable(&hw);
	zassert_equal(core[4 / 4] & BIT(6), 0);
	zassert_equal(core[0x10 / 4] | core[0x14 / 4] | core[0x1c / 4], 0);
	zassert_equal(core[0x400 / 4] & BIT(13), 0);
}

ZTEST_SUITE(ws9652a_usb_hw, NULL, NULL, reset, NULL, NULL);
