/* SPDX-License-Identifier: Apache-2.0 */
#define DT_DRV_COMPAT zgmicro_ws9652a_mbox
#include <zephyr/drivers/mbox.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/sys_io.h>
#include <errno.h>
struct config {
	uintptr_t address;
};
static int send(const struct device *dev, mbox_channel_id_t channel, const struct mbox_msg *msg)
{
	if (channel != 0) {
		return -EINVAL;
	}
	if (msg != NULL) {
		return -EMSGSIZE;
	}
	const struct config *config = dev->config;
	/* Caller owns publication barriers and shared-memory lock/IRQ ordering. */
	sys_write32(sys_read32(config->address) | BIT(0), config->address);
	return 0;
}
static int mtu(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}
static uint32_t channels(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 1;
}
static int callback(const struct device *dev, mbox_channel_id_t channel, mbox_callback_t cb,
		    void *user)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	ARG_UNUSED(cb);
	ARG_UNUSED(user);
	return -EINVAL;
}
static int enable(const struct device *dev, mbox_channel_id_t channel, bool enabled)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	ARG_UNUSED(enabled);
	return -EINVAL;
}
static DEVICE_API(mbox, api) = {
	.send = send,
	.mtu_get = mtu,
	.max_channels_get = channels,
	.register_callback = callback,
	.set_enabled = enable,
};
#define DEFINE(inst)                                                                               \
	static const struct config config_##inst = {.address = DT_INST_REG_ADDR(inst)};            \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, NULL, &config_##inst, PRE_KERNEL_1, 40, &api);
DT_INST_FOREACH_STATUS_OKAY(DEFINE)
