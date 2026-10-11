/* SPDX-License-Identifier: Apache-2.0 */
#define DT_DRV_COMPAT zgmicro_ws9652a_watchdog

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define WDT_CONTROL     0x00u
#define WDT_IRQ_MASK    0x3cu
#define WDT_UPDATE      0x40u
#define WDT_ENABLE      BIT(12)
#define WDT_IRQ_MASKED  BIT(3)
#define WDT_UPDATE_GATE BIT(0)

struct ws9652a_wdt_config {
	uintptr_t base;
};

static int ws9652a_wdt_disable(const struct device *dev)
{
	const struct ws9652a_wdt_config *config = dev->config;
	unsigned int key = irq_lock();
	uint32_t control;

	/* BT 0x8770c / TOP 0x4a84c. The timer channels share these registers;
	 * preserve every bit outside the watchdog stop sequence, including
	 * timer0's enable. Local IRQ exclusion does not arbitrate with BT.
	 * No device data, logging or scheduler dependency: recovery also calls
	 * this before BSS and device initialization, with interrupts masked.
	 */
	sys_write32(sys_read32(config->base + WDT_UPDATE) & ~WDT_UPDATE_GATE,
		    config->base + WDT_UPDATE);
	sys_write32(sys_read32(config->base + WDT_IRQ_MASK) | WDT_IRQ_MASKED,
		    config->base + WDT_IRQ_MASK);
	control = sys_read32(config->base + WDT_CONTROL);
	sys_write32(control & ~WDT_ENABLE, config->base + WDT_CONTROL);
	sys_write32(sys_read32(config->base + WDT_UPDATE) | WDT_UPDATE_GATE,
		    config->base + WDT_UPDATE);
	irq_unlock(key);

	return (control & WDT_ENABLE) ? 0 : -EFAULT;
}

static int ws9652a_wdt_setup(const struct device *dev, uint8_t options)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(options);
	return -ENOTSUP;
}

static int ws9652a_wdt_install_timeout(const struct device *dev, const struct wdt_timeout_cfg *cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cfg);
	return -ENOTSUP;
}

static int ws9652a_wdt_feed(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	return -ENOTSUP;
}

static DEVICE_API(wdt, ws9652a_wdt_api) = {
	.setup = ws9652a_wdt_setup,
	.disable = ws9652a_wdt_disable,
	.install_timeout = ws9652a_wdt_install_timeout,
	.feed = ws9652a_wdt_feed,
};

/* Register before the peer driver (priority 41), without changing hardware. */
#define WDT_WS9652A_DEFINE(inst)                                                                   \
	static const struct ws9652a_wdt_config config_##inst = {                                   \
		.base = DT_INST_REG_ADDR(inst),                                                    \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, NULL, &config_##inst, PRE_KERNEL_1, 40,            \
			      &ws9652a_wdt_api);

DT_INST_FOREACH_STATUS_OKAY(WDT_WS9652A_DEFINE)
