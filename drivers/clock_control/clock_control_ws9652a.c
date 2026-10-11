/* SPDX-License-Identifier: Apache-2.0 */
/* Clock decoding: original TOP 0x44190 / 0x444c8 and table 0x5dbb8.
 * Retained peers own clock transitions; this driver performs no MMIO writes. */
#define DT_DRV_COMPAT zgmicro_ws9652a_clock

#include <zephyr/arch/cpu.h>
#include <zephyr/drivers/clock_control/ws9652a.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define CLOCK_BUS_DIVIDER     0x10u
#define CLOCK_SOURCE          0x40u
#define CLOCK_SELECT          0x50u
#define CLOCK_SOURCE_PLL      BIT(4)
#define CLOCK_PLL_B           BIT(8)
#define CLOCK_PLL_C           BIT(9)
#define CLOCK_INDEX_MASK      GENMASK(3, 0)
#define CLOCK_SECONDARY_SHIFT 2u
#define BUS_PRIMARY_SHIFT     8u
#define BUS_PRIMARY_MASK      GENMASK(2, 0)
#define BUS_SECONDARY_MASK    GENMASK(1, 0)
#define CLOCK_SAMPLE_ATTEMPTS 3u
#define CLOCK_SELECT_MASK     (CLOCK_PLL_B | CLOCK_PLL_C | CLOCK_INDEX_MASK)
#define BUS_DIVIDER_MASK      ((BUS_PRIMARY_MASK << BUS_PRIMARY_SHIFT) | BUS_SECONDARY_MASK)
#define XTAL_NODE(node)       DT_CLOCKS_CTLR_BY_NAME(node, xtal)
#define XTAL_HZ(node)         DT_PROP(XTAL_NODE(node), clock_frequency)

struct ws9652a_clock_control_config {
	uintptr_t base;
	uint32_t xtal_hz;
};

static const uint32_t pll_a_hz[] = {
	96000000u, 128000000u, 96000000u, 0u, 48000000u, 64000000u, 48000000u, 0u, 24000000u,
};
static const uint32_t pll_b_hz[] = {156300000u, 78150000u, 39075000u};
static const uint32_t pll_c_hz[] = {192000000u, 96000000u, 48000000u};

static uint32_t decode_core_clock_hz(uint32_t source, uint32_t select, uint32_t xtal_hz)
{
	uint32_t index;

	if ((source & CLOCK_SOURCE_PLL) == 0u) {
		return xtal_hz;
	}
	index = select & CLOCK_INDEX_MASK;
	if ((select & (CLOCK_PLL_B | CLOCK_PLL_C)) != 0u) {
		index >>= CLOCK_SECONDARY_SHIFT;
		if (index >= ARRAY_SIZE(pll_b_hz)) {
			return 0u;
		}
		return (select & CLOCK_PLL_C) ? pll_c_hz[index] : pll_b_hz[index];
	}
	return index < ARRAY_SIZE(pll_a_hz) ? pll_a_hz[index] : 0u;
}

static int ws9652a_clock_read_rate(uintptr_t base, uint32_t xtal_hz, uintptr_t clock, uint32_t *rate)
{
	if (!rate || (clock != WS9652A_CLOCK_CORE && clock != WS9652A_CLOCK_BUS)) {
		return -EINVAL;
	}
	bool bus = clock == WS9652A_CLOCK_BUS;

	/* A local lock cannot exclude BT's writes. Reject detected relevant field
	 * changes; double sampling does not make the observations atomic. */
	for (unsigned int attempt = 0; attempt < CLOCK_SAMPLE_ATTEMPTS; ++attempt) {
		uint32_t source = sys_read32(base + CLOCK_SOURCE);
		uint32_t select = sys_read32(base + CLOCK_SELECT);
		uint32_t divider = bus ? sys_read32(base + CLOCK_BUS_DIVIDER) : 0;
		uint32_t source_after = sys_read32(base + CLOCK_SOURCE);
		uint32_t select_after = sys_read32(base + CLOCK_SELECT);
		uint32_t divider_after = bus ? sys_read32(base + CLOCK_BUS_DIVIDER) : 0;

		if (((source ^ source_after) & CLOCK_SOURCE_PLL) != 0u ||
		    ((select ^ select_after) & CLOCK_SELECT_MASK) != 0u ||
		    ((divider ^ divider_after) & BUS_DIVIDER_MASK) != 0u) {
			continue;
		}
		uint32_t hz = decode_core_clock_hz(source, select, xtal_hz);

		if (hz == 0u) {
			return -ERANGE;
		}
		if (bus) {
			uint32_t primary = ((divider >> BUS_PRIMARY_SHIFT) & BUS_PRIMARY_MASK) + 1u;
			uint32_t secondary = (divider & BUS_SECONDARY_MASK) + 1u;

			hz /= primary * secondary;
		}
		*rate = hz;
		return 0;
	}
	return -EAGAIN;
}

static int ws9652a_clock_get_rate(const struct device *dev, clock_control_subsys_t clock,
				uint32_t *rate)
{
	const struct ws9652a_clock_control_config *config = dev->config;

	return ws9652a_clock_read_rate(config->base, config->xtal_hz, (uintptr_t)clock, rate);
}

int ws9652a_clock_control_get_rate_early(uint32_t clock, uint32_t *rate)
{
#if DT_NODE_HAS_STATUS(DT_NODELABEL(clkctrl), okay)
	return ws9652a_clock_read_rate(DT_REG_ADDR(DT_NODELABEL(clkctrl)),
				       XTAL_HZ(DT_NODELABEL(clkctrl)), clock, rate);
#else
	ARG_UNUSED(clock);
	ARG_UNUSED(rate);
	return -ENODEV;
#endif
}

int ws9652a_clock_control_get_registers(const struct device *dev,
				      struct ws9652a_clock_registers *out)
{
	if (!dev || !out) {
		return -EINVAL;
	}
	const struct ws9652a_clock_control_config *config = dev->config;
	struct ws9652a_clock_registers registers;

	registers.source = sys_read32(config->base + CLOCK_SOURCE);
	registers.select = sys_read32(config->base + CLOCK_SELECT);
	registers.divider = sys_read32(config->base + CLOCK_BUS_DIVIDER);
	*out = registers;
	return 0;
}

static int ws9652a_clock_gate(const struct device *dev, clock_control_subsys_t clock)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(clock);
	return -ENOTSUP;
}

static int ws9652a_clock_set_rate(const struct device *dev, clock_control_subsys_t clock,
				  clock_control_subsys_rate_t rate)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(clock);
	ARG_UNUSED(rate);
	return -ENOTSUP;
}

static int ws9652a_clock_configure(const struct device *dev, clock_control_subsys_t clock,
				   void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(clock);
	ARG_UNUSED(data);
	return -ENOTSUP;
}

static DEVICE_API(clock_control, ws9652a_clock_api) = {
	.on = ws9652a_clock_gate,
	.off = ws9652a_clock_gate,
	.get_rate = ws9652a_clock_get_rate,
	.set_rate = ws9652a_clock_set_rate,
	.configure = ws9652a_clock_configure,
};

static int ws9652a_clock_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

/* These are recovered absolute PLL rates, not a model of PLL multipliers.
 * Reject other references instead of returning plausible but unverified rates. */
#define WS9652A_CLOCK_DEFINE(inst)                                                                \
	BUILD_ASSERT(DT_NODE_HAS_COMPAT(XTAL_NODE(DT_DRV_INST(inst)), fixed_clock) &&             \
		     DT_NODE_HAS_STATUS(XTAL_NODE(DT_DRV_INST(inst)), okay),                      \
		     "WS9652A requires an enabled fixed-clock reference");                        \
	BUILD_ASSERT(XTAL_HZ(DT_DRV_INST(inst)) == 26000000,                                      \
		     "WS9652A PLL tables support only a 26 MHz crystal reference");               \
	static const struct ws9652a_clock_control_config clock_config_##inst = {                  \
		.base = DT_INST_REG_ADDR(inst),                                                   \
		.xtal_hz = XTAL_HZ(DT_DRV_INST(inst)),                                            \
	};                                                                                        \
	DEVICE_DT_INST_DEFINE(inst, ws9652a_clock_init, NULL, NULL, &clock_config_##inst,         \
			      PRE_KERNEL_1, CONFIG_CLOCK_CONTROL_INIT_PRIORITY, &ws9652a_clock_api);

DT_INST_FOREACH_STATUS_OKAY(WS9652A_CLOCK_DEFINE)
