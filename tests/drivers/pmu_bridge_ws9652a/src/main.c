/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/irq_offload.h>
#include <zephyr/sys/sys_io.h>
#include <cmsis_core.h>

static uint32_t mock_read32(mem_addr_t address);
static void mock_write32(uint32_t value, mem_addr_t address);
#undef DT_INST_FOREACH_STATUS_OKAY
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#define sys_read32                 mock_read32
#define sys_write32                mock_write32
/* The fixture includes two production translation units. Keep their real log
 * modules while separating the logger's otherwise file-local declarations.
 */
#define __log_current_const_data   bus_log_const
#define __log_current_dynamic_data bus_log_dynamic
#define __log_level                bus_log_level
#include "../../../../drivers/misc/ws9652a_bus.c"
#undef __log_current_const_data
#undef __log_current_dynamic_data
#undef __log_level
#undef DT_DRV_COMPAT
#include "../../../../drivers/misc/ws9652a_pmu_bridge.c"
#undef sys_read32
#undef sys_write32

#define BASE 0x40038000u
#define GATE 0x424001ecu
static const struct ws9652a_bus_config bus_config = {.gate = GATE};
static struct ws9652a_bus_data bus_data;
static struct device_state ready = {.initialized = true};
static const struct device bus = {
	.name = "register-bus", .config = &bus_config, .data = &bus_data, .state = &ready};
static const struct ws9652a_pmu_bridge_config config = {.base = BASE, .bus = &bus};
static const struct device bridge = {.name = "pmu-bridge", .config = &config};
static uint32_t registers[6], gate, initial_primask;
static unsigned reads, status_reads, write_count;
static struct {
	uintptr_t address;
	uint32_t value;
} writes[20];
enum response {
	SUCCESS,
	WAIT_SET_TIMEOUT,
	WAIT_CLEAR_TIMEOUT,
	COMPLETE_ERROR,
	WAIT_SET_ERROR
};
static enum response response;

static uint32_t mock_read32(mem_addr_t address)
{
	++reads;
	if (address == GATE) {
		return gate;
	}
	zassert_between_inclusive(address, BASE, BASE + 0x14);
	if (address == BASE + REG_STATUS) {
		++status_reads;
		zassert_true(status_reads <= 6002, "unbounded status polling");
		bool writer_finished =
			!(registers[0] & CONTROL_WRITE) && registers[REG_WRITE_DATA / 4];
		zassert_equal(__get_PRIMASK(), writer_finished ? initial_primask : 1);
		switch (response) {
		case WAIT_SET_TIMEOUT:
			return 0;
		case WAIT_CLEAR_TIMEOUT:
			return STATUS_COMPLETE;
		case WAIT_SET_ERROR:
			return STATUS_ERROR;
		case COMPLETE_ERROR:
			return status_reads == 1 ? STATUS_COMPLETE : STATUS_ERROR;
		default:
			return status_reads == 1 ? STATUS_COMPLETE : 0;
		}
	}
	return registers[(address - BASE) / 4];
}

static void mock_write32(uint32_t value, mem_addr_t address)
{
	zassert_equal_ptr(bus_data.lock.owner, k_current_get());
	zassert_true(write_count < ARRAY_SIZE(writes));
	writes[write_count].address = address;
	writes[write_count++].value = value;
	if (address == GATE) {
		gate = value;
	} else {
		zassert_between_inclusive(address, BASE, BASE + 0x14);
		registers[(address - BASE) / 4] = value;
	}
}

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	ws9652a_bus_init(&bus);
	memset(registers, 0, sizeof(registers));
	registers[0] = 0x401;
	registers[REG_READ_DATA / 4] = 0xa5;
	gate = reads = status_reads = write_count = initial_primask = 0;
	response = SUCCESS;
}

static void assert_cleanup(void)
{
	zassert_equal(registers[0], 0x401);
	zassert_equal(gate, 0);
	zassert_is_null(bus_data.lock.owner);
	zassert_equal(writes[write_count - 3].address, GATE);
	zassert_equal(writes[write_count - 3].value, 0);
	zassert_equal(writes[write_count - 2].value, 0x421);
	zassert_equal(writes[write_count - 1].value, 0x401);
}

ZTEST(pmu_bridge, test_init_getter_and_control_observation)
{
	zassert_ok(ws9652a_pmu_bridge_init(&bridge));
	zassert_equal_ptr(ws9652a_pmu_bridge_bus(&bridge), &bus);
	zassert_equal(reads + write_count, 0);
	zassert_equal(ws9652a_pmu_bridge_control(&bridge), 0x401);
	zassert_equal(reads, 1);
	zassert_equal(write_count, 0);
}

ZTEST(pmu_bridge, test_read_and_write_preserve_native_register_order)
{
	for (unsigned write = 0; write < 2; ++write) {
		reset(NULL);
		uint8_t value = 0x5a;
		uint32_t status = write ? ws9652a_pmu_bridge_write(&bridge, 0xc7, 0x5a)
					: ws9652a_pmu_bridge_read(&bridge, 0xc7, &value);
		zassert_equal(status, 0);
		zassert_equal(value, write ? 0x5a : 0xa5);
		const uintptr_t setup_addresses[] = {BASE, BASE + 0x10, BASE, BASE,
						     BASE, GATE,        BASE};
		const uint32_t setup_values[] = {0x421, 1, 0x461, 0x461, 0x461, 1, 0x461};
		for (unsigned i = 0; i < ARRAY_SIZE(setup_values); ++i) {
			zassert_equal(writes[i].address, setup_addresses[i]);
			zassert_equal(writes[i].value, setup_values[i]);
		}
		unsigned index = 7;
		if (write) {
			zassert_equal(writes[index].address, BASE + REG_WRITE_DATA);
			zassert_equal(writes[index++].value, 0x5a);
		}
		zassert_equal(writes[index].address, BASE + REG_INDEX);
		zassert_equal(writes[index++].value, 0xc7);
		zassert_equal(writes[index++].value, write ? 0x465 : 0x463);
		zassert_equal(writes[index++].value, 0x461);
		zassert_equal(write_count, index + 3);
		zassert_equal(status_reads, 3);
		assert_cleanup();
	}
}

ZTEST(pmu_bridge, test_failures_are_bounded_and_read_output_is_unchanged)
{
	const enum response responses[] = {WAIT_SET_TIMEOUT, WAIT_CLEAR_TIMEOUT, COMPLETE_ERROR,
					   WAIT_SET_ERROR};
	for (unsigned write = 0; write < 2; ++write) {
		for (unsigned i = 0; i < ARRAY_SIZE(responses); ++i) {
			reset(NULL);
			response = responses[i];
			uint8_t value = 0x5a;
			uint32_t expected = (response == COMPLETE_ERROR ||
					     (!write && response == WAIT_SET_ERROR))
						    ? WS9652A_PMU_BRIDGE_STATUS_ERROR
						    : WS9652A_PMU_BRIDGE_TIMEOUT;
			uint32_t status = write ? ws9652a_pmu_bridge_write(&bridge, 5, 0x5a)
						: ws9652a_pmu_bridge_read(&bridge, 5, &value);
			zassert_equal(status, expected);
			zassert_equal(value, 0x5a);
			unsigned expected_reads = response == COMPLETE_ERROR       ? 3
						  : response == WAIT_CLEAR_TIMEOUT ? 3001
						  : write                          ? 3000
										   : 3002;
			zassert_equal(status_reads, expected_reads);
			assert_cleanup();
		}
	}
}

ZTEST(pmu_bridge, test_primask_is_restored_for_success_and_error)
{
	for (unsigned write = 0; write < 2; ++write) {
		for (unsigned mask = 0; mask < 2; ++mask) {
			for (unsigned error = 0; error < 2; ++error) {
				reset(NULL);
				initial_primask = mask;
				response = error ? COMPLETE_ERROR : SUCCESS;
				uint8_t value = 0;
				uint32_t basepri = __get_BASEPRI();
				__set_PRIMASK(mask);
				uint32_t status =
					write ? ws9652a_pmu_bridge_write(&bridge, 5, 0x5a)
					      : ws9652a_pmu_bridge_read(&bridge, 5, &value);
				uint32_t restored = __get_PRIMASK();
				__set_PRIMASK(0);
				zassert_equal(restored, mask);
				zassert_equal(__get_BASEPRI(), basepri);
				zassert_equal(status, error ? WS9652A_PMU_BRIDGE_STATUS_ERROR : 0);
			}
		}
	}
}

ZTEST(pmu_bridge, test_null_output_and_busy_transfer_do_not_write)
{
	zassert_equal(ws9652a_pmu_bridge_read(&bridge, 1, NULL), WS9652A_PMU_BRIDGE_INVALID_OUTPUT);
	zassert_equal(reads + write_count, 0);
	for (unsigned request = CONTROL_READ; request <= TRANSFER_MASK; request += CONTROL_READ) {
		reset(NULL);
		registers[0] |= request;
		uint8_t value = 0x5a;
		zassert_equal(ws9652a_pmu_bridge_read(&bridge, 1, &value), WS9652A_PMU_BRIDGE_BUSY);
		zassert_equal(ws9652a_pmu_bridge_write(&bridge, 1, 2), WS9652A_PMU_BRIDGE_BUSY);
		zassert_equal(value, 0x5a);
		zassert_equal(write_count, 0);
		zassert_is_null(bus_data.lock.owner);
	}
}

ZTEST(pmu_bridge, test_reset_requires_held_window_and_preserves_native_clear_order)
{
	zassert_equal(ws9652a_pmu_bridge_reset(&bridge), WS9652A_PMU_BRIDGE_BUSY);
	zassert_equal(reads + write_count, 0);
	zassert_ok(ws9652a_bus_lock(&bus, false));
	ws9652a_bus_set_peers_held_locked(&bus, true);
	registers[0] = 0x465;
	zassert_equal(ws9652a_pmu_bridge_reset(&bridge), WS9652A_PMU_BRIDGE_BUSY);
	zassert_equal(write_count, 0);
	registers[0] = 0x461;
	zassert_equal(ws9652a_pmu_bridge_reset(&bridge), 0);
	zassert_equal(bus_data.lock.lock_count, 1);
	ws9652a_bus_unlock(&bus);
	const uint32_t expected[] = {0x441, 0x401, 0x400, 1};
	zassert_equal(write_count, ARRAY_SIZE(expected));
	for (unsigned i = 0; i < write_count; ++i) {
		zassert_equal(writes[i].value, expected[i]);
		zassert_equal(writes[i].address, i == 3 ? BASE + REG_DIRECTION : BASE);
	}
	zassert_equal(gate, 0);
}

static void from_isr(const void *unused)
{
	ARG_UNUSED(unused);
	uint8_t value = 0x5a;
	zassert_equal(ws9652a_pmu_bridge_read(&bridge, 1, &value), WS9652A_PMU_BRIDGE_BUSY);
	zassert_equal(ws9652a_pmu_bridge_write(&bridge, 1, 2), WS9652A_PMU_BRIDGE_BUSY);
	zassert_equal(ws9652a_pmu_bridge_reset(&bridge), WS9652A_PMU_BRIDGE_BUSY);
	zassert_equal(value, 0x5a);
}

ZTEST(pmu_bridge, test_isr_refusal_performs_no_mmio)
{
	irq_offload(from_isr, NULL);
	zassert_equal(reads + write_count, 0);
}

ZTEST_SUITE(pmu_bridge, NULL, NULL, reset, NULL, NULL);
