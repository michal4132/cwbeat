/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/irq_offload.h>
#include <zephyr/sys/sys_io.h>

static uint32_t mock_read32(mem_addr_t address);
static void mock_write32(uint32_t value, mem_addr_t address);

#undef DT_INST_FOREACH_STATUS_OKAY
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#define sys_read32  mock_read32
#define sys_write32 mock_write32
#include "../../../../drivers/watchdog/wdt_ws9652a.c"
#undef sys_read32
#undef sys_write32

#define TEST_BASE 0x40018000u

static const struct ws9652a_wdt_config config = {.base = TEST_BASE};
/* No device state or driver data: disable must work before initialization. */
static const struct device watchdog = {.config = &config, .api = &ws9652a_wdt_api};
static uint32_t registers[0x50 / 4];
static struct {
	mem_addr_t address;
	uint32_t value;
	bool write;
} trace[8];
static unsigned int accesses;

static void record(mem_addr_t address, uint32_t value, bool write)
{
	zassert_true(accesses < ARRAY_SIZE(trace), "unbounded watchdog MMIO");
	zassert_true(address >= TEST_BASE && address < TEST_BASE + sizeof(registers));
	zassert_equal(address & 3, 0);
	zassert_false(arch_cpu_irqs_are_enabled(), "protected updates must exclude local IRQs");
	trace[accesses].address = address;
	trace[accesses].value = value;
	trace[accesses++].write = write;
}

static uint32_t mock_read32(mem_addr_t address)
{
	zassert_true(address >= TEST_BASE && address < TEST_BASE + sizeof(registers));
	uint32_t value = registers[(address - TEST_BASE) / 4];

	record(address, value, false);
	return value;
}

static void mock_write32(uint32_t value, mem_addr_t address)
{
	record(address, value, true);
	registers[(address - TEST_BASE) / 4] = value;
}

static void seed(uint32_t pattern)
{
	for (size_t i = 0; i < ARRAY_SIZE(registers); ++i) {
		registers[i] = pattern;
	}
	accesses = 0;
}

static void assert_stopped(uint32_t pattern)
{
	/* Independent oracle for the stock four-write sequence. */
	const uint32_t offsets[] = {0x40, 0x3c, 0x00, 0x40};
	const uint32_t values[] = {pattern & ~1u, pattern | 8u, pattern & ~0x1000u, pattern | 1u};

	zassert_equal(accesses, 8);
	for (size_t i = 0; i < ARRAY_SIZE(offsets); ++i) {
		zassert_false(trace[2 * i].write);
		zassert_equal(trace[2 * i].address, TEST_BASE + offsets[i]);
		zassert_true(trace[2 * i + 1].write);
		zassert_equal(trace[2 * i + 1].address, TEST_BASE + offsets[i]);
		zassert_equal(trace[2 * i + 1].value, values[i]);
	}
	for (size_t i = 0; i < ARRAY_SIZE(registers); ++i) {
		uint32_t expected = pattern;

		switch (4 * i) {
		case 0x00:
			expected &= ~0x1000u;
			break;
		case 0x3c:
			expected |= 8u;
			break;
		case 0x40:
			expected |= 1u;
			break;
		}
		zassert_equal(registers[i], expected, "unrelated timer/watchdog bits changed");
	}
}

ZTEST(wdt_ws9652a, test_disable_preserves_timers_and_stock_order)
{
	const uint32_t patterns[] = {0, UINT32_MAX, 0xaaaaaaaa, 0x55555555};

	for (size_t i = 0; i < ARRAY_SIZE(patterns); ++i) {
		seed(patterns[i]);
		zassert_equal(wdt_disable(&watchdog), patterns[i] & 0x1000 ? 0 : -EFAULT);
		zassert_true(arch_cpu_irqs_are_enabled());
		assert_stopped(patterns[i]);
		accesses = 0;
		zassert_equal(wdt_disable(&watchdog), -EFAULT);
		zassert_equal(accesses, 8, "repeat stop must still normalize mask and update gate");
	}
}

ZTEST(wdt_ws9652a, test_disable_preserves_masked_context)
{
	seed(UINT32_MAX);
	unsigned int key = irq_lock();
	int result = wdt_disable(&watchdog);
	bool masked = !arch_cpu_irqs_are_enabled();

	irq_unlock(key);
	zassert_ok(result);
	zassert_true(masked);
	assert_stopped(UINT32_MAX);
}

static void disable_in_isr(const void *unused)
{
	ARG_UNUSED(unused);
	zassert_ok(wdt_disable(&watchdog));
}

ZTEST(wdt_ws9652a, test_disable_from_isr)
{
	seed(UINT32_MAX);
	irq_offload(disable_in_isr, NULL);
	assert_stopped(UINT32_MAX);
}

ZTEST(wdt_ws9652a, test_unsupported_operations_leave_hardware_unchanged)
{
	const struct wdt_timeout_cfg timeout = {
		.window = {.min = 0, .max = 1000},
		.flags = WDT_FLAG_RESET_SOC,
	};

	seed(UINT32_MAX);
	zassert_equal(wdt_install_timeout(&watchdog, &timeout), -ENOTSUP);
	zassert_equal(wdt_setup(&watchdog, 0), -ENOTSUP);
	zassert_equal(wdt_feed(&watchdog, 0), -ENOTSUP);
	zassert_equal(accesses, 0);
}

ZTEST_SUITE(wdt_ws9652a, NULL, NULL, NULL, NULL, NULL);
