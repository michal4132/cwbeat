/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

static uint32_t ticks, reads, increment, control, trace;
static uintptr_t addresses[16];
static uint32_t mock_read(uintptr_t address);

#define sys_read32 mock_read
#include <zephyr/soc/ws9652a/counter.h>

static uint32_t mock_read(uintptr_t address)
{
	zassert_true(reads < ARRAY_SIZE(addresses));
	addresses[reads++] = address;
	switch (address) {
	case WS9652A_DWT_CYCLES:
		ticks += increment;
		return ticks;
	case WS9652A_DWT_CONTROL:
		return control;
	case WS9652A_DEMCR:
		return trace;
	default:
		zassert_unreachable("unexpected register read");
		return 0;
	}
}

ZTEST(soc_helpers, test_counter_enabled_only_reads_control)
{
	const uint32_t values[] = {0, WS9652A_DWT_ENABLED, WS9652A_DWT_UNAVAILABLE,
				   WS9652A_DWT_ENABLED | WS9652A_DWT_UNAVAILABLE,
				   WS9652A_DWT_ENABLED | BIT(5)};
	const bool expected[] = {false, true, false, false, true};
	for (unsigned i = 0; i < ARRAY_SIZE(values); ++i) {
		control = values[i];
		trace = 0;
		reads = 0;
		zassert_equal(ws9652a_counter_enabled(), expected[i]);
		zassert_equal(reads, 1);
		zassert_equal(addresses[0], WS9652A_DWT_CONTROL);
	}
}

ZTEST(soc_helpers, test_counter_availability_checks_trace_before_control)
{
	control = WS9652A_DWT_ENABLED;
	trace = 0;
	reads = 0;
	zassert_false(ws9652a_counter_available());
	zassert_equal(reads, 1);
	zassert_equal(addresses[0], WS9652A_DEMCR);
	trace = WS9652A_TRACE_ENABLE;
	reads = 0;
	zassert_true(ws9652a_counter_available());
	zassert_equal(reads, 2);
	zassert_equal(addresses[0], WS9652A_DEMCR);
	zassert_equal(addresses[1], WS9652A_DWT_CONTROL);
}

ZTEST(soc_helpers, test_counter_wrap_and_stall_are_bounded)
{
	ticks = UINT32_MAX - 3;
	increment = 1;
	reads = 0;
	zassert_true(ws9652a_counter_wait(8, 10));
	zassert_equal(reads, 9);
	increment = 0;
	reads = 0;
	zassert_false(ws9652a_counter_wait(8, 10));
	zassert_equal(reads, 11);
}

ZTEST_SUITE(soc_helpers, NULL, NULL, NULL, NULL, NULL);
