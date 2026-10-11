/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/drivers/clock_control/ws9652a.h>
#include <zephyr/sys/sys_io.h>

static uint32_t mock_read32(mem_addr_t address);
static void mock_write32(uint32_t value, mem_addr_t address);

#undef DT_INST_FOREACH_STATUS_OKAY
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#define sys_read32  mock_read32
#define sys_write32 mock_write32
#include "../../../../drivers/clock_control/clock_control_ws9652a.c"
#undef sys_read32
#undef sys_write32

enum {
	TEST_SOURCE = 0x40,
	TEST_SELECT = 0x50,
	TEST_DIVIDER = 0x10,
	TRACE_CAPACITY = 32,
};

#define TEST_BASE     DT_REG_ADDR(DT_NODELABEL(clkctrl))
#define RATE_SENTINEL UINT32_C(0xa5a5a5a5)

static const struct ws9652a_clock_control_config config = {
	.base = TEST_BASE,
	.xtal_hz = DT_PROP(DT_NODELABEL(xtal), clock_frequency),
};
static const struct device clock_dev = {.config = &config, .api = &ws9652a_clock_api};
static struct ws9652a_clock_registers registers;
static uintptr_t active_base;
static uintptr_t read_addresses[TRACE_CAPACITY];
static unsigned read_count, write_count, script_count;
static bool scripted;
static struct {
	uintptr_t address;
	uint32_t value;
} script[TRACE_CAPACITY];

static uint32_t mock_read32(mem_addr_t address)
{
	zassert_true(read_count < ARRAY_SIZE(read_addresses), "unbounded clock reads");
	read_addresses[read_count] = address;
	if (scripted) {
		zassert_true(read_count < script_count, "read after scripted observations");
		zassert_equal(address, script[read_count].address, "clock register read order");
		return script[read_count++].value;
	}
	++read_count;
	switch (address - active_base) {
	case TEST_SOURCE:
		return registers.source;
	case TEST_SELECT:
		return registers.select;
	case TEST_DIVIDER:
		return registers.divider;
	default:
		zassert_unreachable("unexpected clock register address");
		return 0;
	}
}

static void __unused mock_write32(uint32_t value, mem_addr_t address)
{
	ARG_UNUSED(value);
	ARG_UNUSED(address);
	++write_count;
	zassert_unreachable("clock provider must never write registers");
}

static void clear_trace(void)
{
	read_count = write_count = script_count = 0;
	scripted = false;
}

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	registers = (struct ws9652a_clock_registers){0};
	active_base = TEST_BASE;
	clear_trace();
}

static int read_rate(bool early, uint32_t clock, uint32_t *rate)
{
	return early ? ws9652a_clock_control_get_rate_early(clock, rate)
		     : clock_control_get_rate(&clock_dev, (clock_control_subsys_t)(uintptr_t)clock,
					      rate);
}

static void assert_read_order(uint32_t clock, unsigned attempts)
{
	const uintptr_t order[] = {active_base + TEST_SOURCE, active_base + TEST_SELECT,
				   active_base + TEST_DIVIDER};
	unsigned registers_per_sample = clock == WS9652A_CLOCK_BUS ? 3 : 2;

	zassert_equal(read_count, 2 * attempts * registers_per_sample);
	for (unsigned i = 0; i < read_count; ++i) {
		zassert_equal(read_addresses[i], order[i % registers_per_sample]);
	}
	zassert_equal(write_count, 0);
}

static void expect_rate(uint32_t clock, uint32_t expected)
{
	/* Exercise the standard driver API and the stateless early entry point. */
	for (unsigned early = 0; early < 2; ++early) {
		uint32_t rate = RATE_SENTINEL;

		clear_trace();
		zassert_ok(read_rate(early, clock, &rate));
		zassert_equal(rate, expected);
		assert_read_order(clock, 1);
	}
}

static void expect_invalid_rate(uint32_t clock)
{
	for (unsigned early = 0; early < 2; ++early) {
		uint32_t rate = RATE_SENTINEL;

		clear_trace();
		zassert_equal(read_rate(early, clock, &rate), -ERANGE);
		zassert_equal(rate, RATE_SENTINEL);
		assert_read_order(clock, 1);
	}
}

ZTEST(clock_control_ws9652a, test_crystal_ignores_pll_encoding)
{
	registers.source = BIT(31);
	registers.select = UINT32_MAX;
	expect_rate(WS9652A_CLOCK_CORE, 26000000);
	expect_rate(WS9652A_CLOCK_BUS, 26000000);
}

ZTEST(clock_control_ws9652a, test_pll_a_table_and_reserved_encodings)
{
	const uint32_t expected[] = {
		96000000, 128000000, 96000000, 0, 48000000, 64000000, 48000000, 0,
		24000000, 0,         0,        0, 0,        0,        0,        0,
	};

	registers.source = BIT(4);
	for (unsigned i = 0; i < ARRAY_SIZE(expected); ++i) {
		registers.select = i;
		if (expected[i]) {
			expect_rate(WS9652A_CLOCK_CORE, expected[i]);
		} else {
			expect_invalid_rate(WS9652A_CLOCK_CORE);
			expect_invalid_rate(WS9652A_CLOCK_BUS);
		}
	}
}

ZTEST(clock_control_ws9652a, test_pll_b_c_tables_and_c_priority)
{
	const uint32_t expected_b[] = {156300000, 78150000, 39075000};
	const uint32_t expected_c[] = {192000000, 96000000, 48000000};
	const uint32_t selectors[] = {BIT(8), BIT(9), BIT(8) | BIT(9)};

	registers.source = BIT(4);
	for (unsigned family = 0; family < ARRAY_SIZE(selectors); ++family) {
		for (unsigned index = 0; index < 16; ++index) {
			registers.select = selectors[family] | index;
			if (index < 12) {
				expect_rate(WS9652A_CLOCK_CORE,
					    family ? expected_c[index / 4] : expected_b[index / 4]);
			} else {
				expect_invalid_rate(WS9652A_CLOCK_CORE);
				expect_invalid_rate(WS9652A_CLOCK_BUS);
			}
		}
	}
}

ZTEST(clock_control_ws9652a, test_both_bus_dividers_and_integer_truncation)
{
	registers.source = BIT(4);
	registers.select = BIT(8);
	for (unsigned primary = 1; primary <= 8; ++primary) {
		for (unsigned secondary = 1; secondary <= 4; ++secondary) {
			registers.divider = ((primary - 1) << 8) | (secondary - 1);
			expect_rate(WS9652A_CLOCK_BUS, 156300000 / (primary * secondary));
		}
	}
	/* The core query does not consult either bus divider. */
	registers.divider = UINT32_MAX;
	expect_rate(WS9652A_CLOCK_CORE, 156300000);
}

static void append_snapshot(uint32_t clock, struct ws9652a_clock_registers snapshot)
{
	scripted = true;
	script[script_count].address = TEST_BASE + TEST_SOURCE;
	script[script_count++].value = snapshot.source;
	script[script_count].address = TEST_BASE + TEST_SELECT;
	script[script_count++].value = snapshot.select;
	if (clock == WS9652A_CLOCK_BUS) {
		script[script_count].address = TEST_BASE + TEST_DIVIDER;
		script[script_count++].value = snapshot.divider;
	}
}

ZTEST(clock_control_ws9652a, test_relevant_changes_retry_then_publish_stable_rate)
{
	const struct ws9652a_clock_registers old = {BIT(4), 0, 0};
	const struct ws9652a_clock_registers changed[] = {
		{0, 0, 0},           {BIT(4), 1, 0},      {BIT(4), BIT(8), 0},
		{BIT(4), BIT(9), 0}, {BIT(4), 0, BIT(8)}, {BIT(4), 0, 1},
	};
	const struct ws9652a_clock_registers stable = {BIT(4), BIT(9), 0x102};

	for (uint32_t clock = WS9652A_CLOCK_CORE; clock <= WS9652A_CLOCK_BUS; ++clock) {
		for (unsigned field = 0; field < ARRAY_SIZE(changed); ++field) {
			if (clock == WS9652A_CLOCK_CORE && field >= 4) {
				continue;
			}
			for (unsigned early = 0; early < 2; ++early) {
				uint32_t rate = RATE_SENTINEL;

				clear_trace();
				append_snapshot(clock, old);
				append_snapshot(clock, changed[field]);
				append_snapshot(clock, stable);
				append_snapshot(clock, stable);
				zassert_ok(read_rate(early, clock, &rate));
				zassert_equal(rate,
					      clock == WS9652A_CLOCK_CORE ? 192000000 : 32000000);
				assert_read_order(clock, 2);
			}
		}
	}
}

ZTEST(clock_control_ws9652a, test_retry_exhaustion_preserves_output)
{
	const struct ws9652a_clock_registers old = {BIT(4), 0, 0};
	const struct ws9652a_clock_registers changed = {BIT(4), 1, 0};

	for (uint32_t clock = WS9652A_CLOCK_CORE; clock <= WS9652A_CLOCK_BUS; ++clock) {
		for (unsigned early = 0; early < 2; ++early) {
			uint32_t rate = RATE_SENTINEL;

			clear_trace();
			for (unsigned attempt = 0; attempt < 3; ++attempt) {
				append_snapshot(clock, old);
				append_snapshot(clock, changed);
			}
			zassert_equal(read_rate(early, clock, &rate), -EAGAIN);
			zassert_equal(rate, RATE_SENTINEL);
			assert_read_order(clock, 3);
		}
	}
}

ZTEST(clock_control_ws9652a, test_irrelevant_bits_do_not_force_retries)
{
	const struct ws9652a_clock_registers old = {BIT(4), BIT(9), 0x102};
	const struct ws9652a_clock_registers changed = {
		BIT(4) | BIT(31),
		BIT(9) | BIT(31) | BIT(7),
		0x102 | BIT(31) | BIT(7),
	};

	for (uint32_t clock = WS9652A_CLOCK_CORE; clock <= WS9652A_CLOCK_BUS; ++clock) {
		for (unsigned early = 0; early < 2; ++early) {
			uint32_t rate = RATE_SENTINEL;

			clear_trace();
			append_snapshot(clock, old);
			append_snapshot(clock, changed);
			zassert_ok(read_rate(early, clock, &rate));
			zassert_equal(rate, clock == WS9652A_CLOCK_CORE ? 192000000 : 32000000);
			assert_read_order(clock, 1);
		}
	}
}

ZTEST(clock_control_ws9652a, test_invalid_arguments_perform_no_reads)
{
	const uint32_t invalid[] = {0, 3, UINT32_MAX};
	uint32_t rate = RATE_SENTINEL;
	struct ws9652a_clock_registers snapshot = {1, 2, 3};
	const struct ws9652a_clock_registers original = snapshot;

	for (unsigned early = 0; early < 2; ++early) {
		for (unsigned i = 0; i < ARRAY_SIZE(invalid); ++i) {
			zassert_equal(read_rate(early, invalid[i], &rate), -EINVAL);
		}
		zassert_equal(read_rate(early, WS9652A_CLOCK_CORE, NULL), -EINVAL);
		zassert_equal(read_rate(early, WS9652A_CLOCK_BUS, NULL), -EINVAL);
	}
	zassert_equal(rate, RATE_SENTINEL);
	zassert_equal(ws9652a_clock_control_get_registers(&clock_dev, NULL), -EINVAL);
	zassert_equal(ws9652a_clock_control_get_registers(NULL, &snapshot), -EINVAL);
	zassert_mem_equal(&snapshot, &original, sizeof(snapshot));
	zassert_equal(read_count, 0);
	zassert_equal(write_count, 0);
}

ZTEST(clock_control_ws9652a, test_snapshot_preserves_raw_bits_and_read_order)
{
	struct ws9652a_clock_registers snapshot;

	registers = (struct ws9652a_clock_registers){0x80000010, 0xabcdef09, 0x76543210};
	zassert_ok(ws9652a_clock_control_get_registers(&clock_dev, &snapshot));
	zassert_mem_equal(&snapshot, &registers, sizeof(snapshot));
	zassert_equal(read_count, 3);
	zassert_equal(read_addresses[0], TEST_BASE + TEST_SOURCE);
	zassert_equal(read_addresses[1], TEST_BASE + TEST_SELECT);
	zassert_equal(read_addresses[2], TEST_BASE + TEST_DIVIDER);
	zassert_equal(write_count, 0);
}

ZTEST(clock_control_ws9652a, test_runtime_uses_device_base_and_early_uses_devicetree)
{
	const struct ws9652a_clock_control_config alternate_config = {
		.base = TEST_BASE + 0x1000,
		.xtal_hz = DT_PROP(DT_NODELABEL(xtal), clock_frequency),
	};
	const struct device alternate = {.config = &alternate_config, .api = &ws9652a_clock_api};
	struct ws9652a_clock_registers snapshot;
	uint32_t rate = RATE_SENTINEL;

	active_base = alternate_config.base;
	zassert_ok(clock_control_get_rate(
		&alternate, (clock_control_subsys_t)(uintptr_t)WS9652A_CLOCK_CORE, &rate));
	zassert_equal(rate, 26000000);
	assert_read_order(WS9652A_CLOCK_CORE, 1);
	clear_trace();
	zassert_ok(ws9652a_clock_control_get_registers(&alternate, &snapshot));
	zassert_equal(read_count, 3);
	zassert_equal(read_addresses[0], active_base + TEST_SOURCE);
	zassert_equal(read_addresses[1], active_base + TEST_SELECT);
	zassert_equal(read_addresses[2], active_base + TEST_DIVIDER);
	zassert_mem_equal(&snapshot, &registers, sizeof(snapshot));
	active_base = TEST_BASE;
	clear_trace();
	zassert_ok(ws9652a_clock_control_get_rate_early(WS9652A_CLOCK_CORE, &rate));
	zassert_equal(rate, 26000000);
	assert_read_order(WS9652A_CLOCK_CORE, 1);
}

static void unexpected_callback(const struct device *dev, clock_control_subsys_t clock,
				void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(clock);
	ARG_UNUSED(user_data);
	zassert_unreachable("read-only clock provider must never start a clock");
}

ZTEST(clock_control_ws9652a, test_mutation_and_status_requests_do_not_touch_hardware)
{
	const uint32_t clocks[] = {WS9652A_CLOCK_CORE, WS9652A_CLOCK_BUS, 0, UINT32_MAX};

	zassert_ok(ws9652a_clock_init(&clock_dev));
	for (unsigned i = 0; i < ARRAY_SIZE(clocks); ++i) {
		clock_control_subsys_t clock = (clock_control_subsys_t)(uintptr_t)clocks[i];

		zassert_equal(clock_control_on(&clock_dev, clock), -ENOTSUP);
		zassert_equal(clock_control_off(&clock_dev, clock), -ENOTSUP);
		zassert_equal(
			clock_control_set_rate(&clock_dev, clock, (void *)(uintptr_t)96000000),
			-ENOTSUP);
		zassert_equal(clock_control_configure(&clock_dev, clock, NULL), -ENOTSUP);
		zassert_equal(clock_control_async_on(&clock_dev, clock, unexpected_callback, NULL),
			      -ENOSYS);
		zassert_equal(clock_control_get_status(&clock_dev, clock),
			      CLOCK_CONTROL_STATUS_UNKNOWN);
	}
	zassert_equal(read_count, 0);
	zassert_equal(write_count, 0);
}

ZTEST_SUITE(clock_control_ws9652a, NULL, NULL, reset, NULL, NULL);
