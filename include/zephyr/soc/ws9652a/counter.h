#ifndef ZGMICRO_WS9652A_COUNTER_H
#define ZGMICRO_WS9652A_COUNTER_H
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#define WS9652A_DEMCR           UINT32_C(0xe000edfc)
#define WS9652A_TRACE_ENABLE    (UINT32_C(1) << 24)
#define WS9652A_DWT_CONTROL     UINT32_C(0xe0001000)
#define WS9652A_DWT_CYCLES      UINT32_C(0xe0001004)
#define WS9652A_DWT_ENABLED     UINT32_C(1)
#define WS9652A_DWT_UNAVAILABLE (UINT32_C(1) << 25)
static inline bool ws9652a_counter_enable(void)
{
	sys_set_bits(WS9652A_DEMCR, WS9652A_TRACE_ENABLE);
	if (sys_read32(WS9652A_DWT_CONTROL) & WS9652A_DWT_UNAVAILABLE) {
		return false;
	}
	sys_set_bits(WS9652A_DWT_CONTROL, WS9652A_DWT_ENABLED);
	return true;
}
/* Check CYCCNT's own control without reading or changing trace configuration. */
static inline bool ws9652a_counter_enabled(void)
{
	return (sys_read32(WS9652A_DWT_CONTROL) &
		(WS9652A_DWT_UNAVAILABLE | WS9652A_DWT_ENABLED)) == WS9652A_DWT_ENABLED;
}
static inline bool ws9652a_counter_available(void)
{
	return (sys_read32(WS9652A_DEMCR) & WS9652A_TRACE_ENABLE) != 0 &&
	       ws9652a_counter_enabled();
}
static inline uint32_t ws9652a_counter_read(void)
{
	return sys_read32(WS9652A_DWT_CYCLES);
}
/* Bounded even if CYCCNT stops. Caller chooses the validated poll budget. */
static inline bool ws9652a_counter_wait(uint32_t cycles, uint32_t poll_limit)
{
	uint32_t start = ws9652a_counter_read();
	while (poll_limit--) {
		if ((uint32_t)(ws9652a_counter_read() - start) >= cycles) {
			return true;
		}
	}
	return false;
}
#endif
