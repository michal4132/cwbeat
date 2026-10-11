/* SPDX-License-Identifier: Apache-2.0 */
/* Routed controller from stock TOP 0x45198/0x45234 and byte worker 0x4ab40.
 * Both channel devices share the parent engine and register-bus transaction.
 */
#define DT_DRV_COMPAT zgmicro_ws9652a_i2c

#include <errno.h>
#include <string.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/clock_control/ws9652a.h>
#include <zephyr/drivers/i2c/ws9652a.h>
#include <zephyr/drivers/misc/ws9652a_bus.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/soc/ws9652a/counter.h>
#include <zephyr/sys/sys_io.h>

LOG_MODULE_REGISTER(i2c_ws9652a, CONFIG_I2C_WS9652A_LOG_LEVEL);

enum {
	REG_CONTROL = 0x00,
	REG_COMMAND = 0x04,
	REG_TX = 0x08,
	REG_RX = 0x0c,
	REG_STATUS = 0x10,
	REG_ROUTE = 0x20,
	CONTROL_ENABLE = BIT(27),
	COMMAND_START = 0x28,
	COMMAND_WRITE = 8,
	COMMAND_READ = 4,
	COMMAND_READ_LAST = 6,
	COMMAND_STOP = 0x10,
	STATUS_BUSY = BIT(0),
	STATUS_STOP_BUSY = BIT(1),
	STATUS_NACK = BIT(2),
	STATUS_ARBITRATION = BIT(3),
	STATUS_STOP_COMPLETE = BIT(8),
	TRANSFER_POLLS = 0x3000 - 1,
	TRANSFER_DELAY_US = 10,
	STOP_POLLS = 500,
	MAX_TRANSFER_BYTES = 256,
	CLOCK_DIVISOR = 800000,
	MINIMUM_DIVIDER = 2,
};

struct i2c_ws9652a_config {
	uintptr_t base;
	const struct device *clock;
	clock_control_subsys_t core_clock;
	clock_control_subsys_t bus_clock;
	const struct device *bus;
};

struct i2c_ws9652a_data {
	/* Shared across both channels and protected by the register-bus lock. */
	uint8_t received[MAX_TRANSFER_BYTES];
};

struct i2c_ws9652a_channel_config {
	const struct device *parent;
	const struct pinctrl_dev_config *pinctrl;
	uint8_t route;
};

struct i2c_ws9652a_channel_data {
	bool pins_applied;
};

static const struct i2c_ws9652a_config *parent_config(const struct device *dev)
{
	const struct i2c_ws9652a_channel_config *channel = dev->config;

	return channel->parent->config;
}

const struct device *ws9652a_i2c_bus(const struct device *dev)
{
	return parent_config(dev)->bus;
}

static int thread_context(void)
{
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	return k_is_pre_kernel() ? -EAGAIN : 0;
}

static int i2c_ws9652a_configure(const struct device *dev, uint32_t config)
{
	int error =
		config == (I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD)) ? 0 : -ENOTSUP;

	LOG_DBG("%d: %s: configure: 0x%08x", error, dev->name, config);
	return error;
}

static int i2c_ws9652a_get_config(const struct device *dev, uint32_t *config)
{
	ARG_UNUSED(dev);
	if (!config) {
		return -EINVAL;
	}
	*config = I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD);
	return 0;
}

/* Support a single read/write, a repeated-START write/read pair, and a
 * contiguous pair of write buffers (i2c_burst_write's register and payload).
 * A complete transaction must end in STOP. Validate the whole request before
 * entering the shared transaction or touching hardware.
 */
static int validate_messages(struct i2c_msg *msgs, uint8_t count, uint16_t address)
{
	if (!msgs || !count || address > 0x7f) {
		return -EINVAL;
	}
	if (count > 2) {
		return -ENOTSUP;
	}
	uint32_t total = 0;
	for (unsigned i = 0; i < count; ++i) {
		if (!msgs[i].buf || !msgs[i].len) {
			return -EINVAL;
		}
		if (msgs[i].flags & ~(I2C_MSG_READ | I2C_MSG_STOP | I2C_MSG_RESTART)) {
			return -ENOTSUP;
		}
		if (msgs[i].len > MAX_TRANSFER_BYTES - total) {
			return -EMSGSIZE;
		}
		total += msgs[i].len;
	}
	if (!(msgs[count - 1].flags & I2C_MSG_STOP)) {
		return -ENOTSUP;
	}
	if (count == 2) {
		if (msgs[0].flags & (I2C_MSG_READ | I2C_MSG_STOP)) {
			return -ENOTSUP;
		}
		/* A direction change requires repeated START. Same-direction
		 * buffers must continue the current write without another START.
		 */
		bool read = (msgs[1].flags & I2C_MSG_READ) != 0;
		bool restart = (msgs[1].flags & I2C_MSG_RESTART) != 0;
		if (read != restart) {
			return -ENOTSUP;
		}
	}
	return 0;
}

static bool delay_us(uint32_t us, uint32_t cycles_per_us)
{
	uint32_t cycles = us * cycles_per_us;

	return ws9652a_counter_wait(cycles, cycles * 2u);
}

static int wait_ready(const struct i2c_ws9652a_config *config, uint32_t cycles_per_us,
		      unsigned *remaining, bool check_nack)
{
	while (*remaining) {
		--*remaining;
		if (!delay_us(TRANSFER_DELAY_US, cycles_per_us)) {
			return -EIO;
		}
		uint32_t status = sys_read32(config->base + REG_STATUS);

		if (status & STATUS_ARBITRATION) {
			return -EAGAIN;
		}
		if (status & STATUS_BUSY) {
			continue;
		}
		if (check_nack && (status & STATUS_NACK)) {
			return -ENXIO;
		}
		return 0;
	}
	return -ETIMEDOUT;
}

static int send_start(const struct i2c_ws9652a_config *config, uint16_t address, bool read,
		      uint32_t cycles_per_us, unsigned *remaining)
{
	sys_write32((address << 1) | read, config->base + REG_TX);
	sys_write32(COMMAND_START, config->base + REG_COMMAND);
	return wait_ready(config, cycles_per_us, remaining, true);
}

static int stop_wait(const struct i2c_ws9652a_config *config, uint32_t cycles_per_us)
{
	sys_write32(sys_read32(config->base + REG_COMMAND) | COMMAND_STOP,
		    config->base + REG_COMMAND);
	for (unsigned remaining = STOP_POLLS; remaining; --remaining) {
		if (!(sys_read32(config->base + REG_STATUS) & STATUS_STOP_BUSY)) {
			for (unsigned complete = STOP_POLLS; complete; --complete) {
				if (sys_read32(config->base + REG_STATUS) & STATUS_STOP_COMPLETE) {
					return 0;
				}
				if (!delay_us(1, cycles_per_us)) {
					return -EIO;
				}
			}
			return -ETIMEDOUT;
		}
		if (!delay_us(1, cycles_per_us)) {
			return -EIO;
		}
	}
	return -ETIMEDOUT;
}

static int transfer_locked(const struct device *dev, struct i2c_msg *msgs, uint8_t count,
			   uint16_t address, struct ws9652a_i2c_result *result)
{
	const struct i2c_ws9652a_channel_config *channel = dev->config;
	struct i2c_ws9652a_channel_data *channel_data = dev->data;
	const struct i2c_ws9652a_config *config = channel->parent->config;
	struct i2c_ws9652a_data *data = channel->parent->data;
	uint32_t core_hz, bus_hz;
	int error = clock_control_get_rate(config->clock, config->core_clock, &core_hz);

	if (error) {
		return error;
	}
	error = clock_control_get_rate(config->clock, config->bus_clock, &bus_hz);
	if (error) {
		return error;
	}
	if (!core_hz || !bus_hz) {
		return -ERANGE;
	}
	if (!ws9652a_counter_enabled()) {
		return -EIO;
	}
	if ((sys_read32(config->base + REG_STATUS) & (STATUS_BUSY | STATUS_STOP_BUSY)) ||
	    ws9652a_bus_gate_get_locked(config->bus)) {
		return -EBUSY;
	}
	if (channel->pinctrl && !channel_data->pins_applied) {
		error = pinctrl_apply_state(channel->pinctrl, PINCTRL_STATE_DEFAULT);
		if (error) {
			return error;
		}
		channel_data->pins_applied = true;
	}
	uint32_t cycles_per_us = core_hz / 1000000u + (core_hz % 1000000u != 0);
	uint32_t divider = bus_hz / CLOCK_DIVISOR;

	divider = divider > MINIMUM_DIVIDER ? divider - 1u : MINIMUM_DIVIDER;
	ws9652a_bus_gate_set_locked(config->bus, 1);
	sys_write32(divider | CONTROL_ENABLE, config->base + REG_CONTROL);
	sys_write32(channel->route, config->base + REG_ROUTE);
	unsigned remaining = TRANSFER_POLLS;
	unsigned received = 0;
	for (unsigned m = 0; m < count; ++m) {
		struct i2c_msg *msg = &msgs[m];
		bool read = (msg->flags & I2C_MSG_READ) != 0;

		result->message_index = m;
		if (m == 0 || (msg->flags & I2C_MSG_RESTART)) {
			result->phase = WS9652A_I2C_PHASE_ADDRESS;
			error = send_start(config, address, read, cycles_per_us, &remaining);
			if (error) {
				break;
			}
		}
		result->phase = WS9652A_I2C_PHASE_DATA;
		for (unsigned i = 0; i < msg->len; ++i) {
			if (read) {
				sys_write32(i + 1 == msg->len ? COMMAND_READ_LAST : COMMAND_READ,
					    config->base + REG_COMMAND);
			} else {
				sys_write32(msg->buf[i], config->base + REG_TX);
				sys_write32(COMMAND_WRITE, config->base + REG_COMMAND);
			}
			error = wait_ready(config, cycles_per_us, &remaining, !read);
			if (error) {
				break;
			}
			if (read) {
				data->received[received++] = sys_read32(config->base + REG_RX);
			}
			++result->data_completed;
		}
		if (error) {
			break;
		}
	}
	/* No replay: a write can have taken effect despite a lost ACK or STOP. */
	if (!error) {
		result->phase = WS9652A_I2C_PHASE_STOP;
	}
	result->stop_error = stop_wait(config, cycles_per_us);
	sys_write32(sys_read32(config->base + REG_CONTROL) & ~CONTROL_ENABLE,
		    config->base + REG_CONTROL);
	ws9652a_bus_gate_set_locked(config->bus, 0);
	if (error || result->stop_error) {
		return error;
	}
	received = 0;
	for (unsigned m = 0; m < count; ++m) {
		struct i2c_msg *msg = &msgs[m];

		if (msg->flags & I2C_MSG_READ) {
			memcpy(msg->buf, data->received + received, msg->len);
			received += msg->len;
		}
	}
	result->phase = WS9652A_I2C_PHASE_COMPLETE;
	return 0;
}

int ws9652a_i2c_transfer_detailed(const struct device *dev, struct i2c_msg *msgs, uint8_t count,
				  uint16_t address, struct ws9652a_i2c_result *result)
{
	struct ws9652a_i2c_result local = {.phase = WS9652A_I2C_PHASE_PREFLIGHT};
	int error = validate_messages(msgs, count, address);

	if (!error) {
		error = thread_context();
	}
	if (!error) {
		const struct device *bus = ws9652a_i2c_bus(dev);

		error = ws9652a_bus_lock(bus, true);
		if (!error) {
			error = transfer_locked(dev, msgs, count, address, &local);
			ws9652a_bus_unlock(bus);
		}
	}
	local.transfer_error = error;
	if (result) {
		*result = local;
	}
	int status = error ? error : local.stop_error;

	if (status) {
		LOG_DBG("%d: %s: route: %u, address: 0x%02x: transfer failed: "
			"msg: %u, phase: %u, status: %d, stop: %d, completed: %zu",
			status, dev->name,
			((const struct i2c_ws9652a_channel_config *)dev->config)->route, address,
			local.message_index, local.phase, local.transfer_error, local.stop_error,
			local.data_completed);
	}
	return status;
}

static int i2c_ws9652a_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t count,
				uint16_t address)
{
	return ws9652a_i2c_transfer_detailed(dev, msgs, count, address, NULL);
}

int ws9652a_i2c_delay(const struct device *dev, uint32_t usec)
{
	int error = thread_context();

	if (error || !usec) {
		return error;
	}
	const struct i2c_ws9652a_config *config = parent_config(dev);

	error = ws9652a_bus_lock(config->bus, false);
	if (error) {
		return error;
	}
	uint32_t core_hz;

	error = clock_control_get_rate(config->clock, config->core_clock, &core_hz);
	if (!error) {
		uint32_t cycles_per_us = core_hz / 1000000u + (core_hz % 1000000u != 0);

		if (!cycles_per_us || usec > UINT32_MAX / 2u / cycles_per_us) {
			error = -ERANGE;
		} else if (!ws9652a_counter_enabled() || !delay_us(usec, cycles_per_us)) {
			error = -EIO;
		}
	}
	ws9652a_bus_unlock(config->bus);
	return error;
}

int ws9652a_i2c_snapshot(const struct device *dev, uint32_t out[8])
{
	if (!out) {
		return -EINVAL;
	}
	int error = thread_context();

	if (error) {
		return error;
	}
	const struct i2c_ws9652a_config *config = parent_config(dev);

	error = ws9652a_bus_lock(config->bus, false);
	if (error) {
		return error;
	}
	struct ws9652a_clock_registers clock;

	error = ws9652a_clock_control_get_registers(config->clock, &clock);
	if (!error) {
		uint32_t snapshot[] = {
			clock.source,
			clock.select,
			clock.divider,
			sys_read32(config->base + REG_CONTROL),
			sys_read32(config->base + REG_COMMAND),
			sys_read32(config->base + REG_STATUS),
			sys_read32(config->base + REG_ROUTE),
			ws9652a_bus_gate_get_locked(config->bus),
		};

		memcpy(out, snapshot, sizeof(snapshot));
	}
	ws9652a_bus_unlock(config->bus);
	return error;
}

static int i2c_ws9652a_init(const struct device *dev)
{
	const struct i2c_ws9652a_config *config = dev->config;

	if (!device_is_ready(config->clock)) {
		LOG_ERR("%d: %s: clock: %s: not ready", -ENODEV, dev->name, config->clock->name);
		return -ENODEV;
	}
	if (!device_is_ready(config->bus)) {
		LOG_ERR("%d: %s: register bus: %s: not ready", -ENODEV, dev->name,
			config->bus->name);
		return -ENODEV;
	}
	LOG_DBG("%s: controller initialized", dev->name);
	return 0;
}

static int i2c_ws9652a_channel_init(const struct device *dev)
{
	const struct i2c_ws9652a_channel_config *config = dev->config;

	if (!device_is_ready(config->parent)) {
		LOG_ERR("%d: %s: controller: %s: not ready", -ENODEV, dev->name,
			config->parent->name);
		return -ENODEV;
	}
	LOG_DBG("%s: route: %u: initialized", dev->name, config->route);
	return 0;
}

static DEVICE_API(i2c, i2c_ws9652a_api) = {
	.configure = i2c_ws9652a_configure,
	.get_config = i2c_ws9652a_get_config,
	.transfer = i2c_ws9652a_transfer,
};

#define I2C_WS9652A_DEFINE(inst)                                                                   \
	BUILD_ASSERT(DT_SAME_NODE(DT_INST_CLOCKS_CTLR_BY_NAME(inst, core),                         \
				  DT_INST_CLOCKS_CTLR_BY_NAME(inst, bus)),                         \
		     "WS9652A core and bus clocks must share a provider");                         \
	BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_INST_CLOCKS_CTLR_BY_NAME(inst, core),                   \
					zgmicro_ws9652a_clock),                                    \
		     "WS9652A raw clock snapshots require its clock provider");                    \
	static const struct i2c_ws9652a_config i2c_config_##inst = {                               \
		.base = DT_INST_REG_ADDR_BY_NAME(inst, controller),                                \
		.clock = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(inst, core)),                   \
		.core_clock = (clock_control_subsys_t)DT_INST_CLOCKS_CELL_BY_NAME(inst, core, id), \
		.bus_clock = (clock_control_subsys_t)DT_INST_CLOCKS_CELL_BY_NAME(inst, bus, id),   \
		.bus = DEVICE_DT_GET(DT_INST_PHANDLE(inst, register_bus)),                         \
	};                                                                                         \
	static struct i2c_ws9652a_data i2c_data_##inst;                                            \
	DEVICE_DT_INST_DEFINE(inst, i2c_ws9652a_init, NULL, &i2c_data_##inst, &i2c_config_##inst,  \
			      PRE_KERNEL_1, 35, NULL);

DT_INST_FOREACH_STATUS_OKAY(I2C_WS9652A_DEFINE)

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT zgmicro_ws9652a_i2c_channel

#define I2C_WS9652A_CHANNEL_DEFINE(inst)                                                           \
	BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_PARENT(DT_DRV_INST(inst)), zgmicro_ws9652a_i2c),        \
		     "WS9652A channels require their controller parent");                          \
	BUILD_ASSERT(DT_INST_REG_ADDR(inst) <= 1, "Only route 0 and 1 are supported");             \
	BUILD_ASSERT(!DT_INST_NODE_HAS_PROP(inst, pinctrl_0) || DT_INST_REG_ADDR(inst) == 0,       \
		     "Only route 0 supports external pinctrl");                                    \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, pinctrl_0), (PINCTRL_DT_INST_DEFINE(inst);), ())                                                                                \
	static const struct i2c_ws9652a_channel_config channel_config_##inst = {                   \
		.parent = DEVICE_DT_GET(DT_PARENT(DT_DRV_INST(inst))),                             \
		.route = DT_INST_REG_ADDR(inst),                                                   \
		.pinctrl = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, pinctrl_0),               \
			(PINCTRL_DT_INST_DEV_CONFIG_GET(inst)), (NULL)),                            \
	};                                                                                         \
	static struct i2c_ws9652a_channel_data channel_data_##inst;                                \
	I2C_DEVICE_DT_INST_DEFINE(inst, i2c_ws9652a_channel_init, NULL, &channel_data_##inst,      \
				  &channel_config_##inst, PRE_KERNEL_1, 36, &i2c_ws9652a_api);

DT_INST_FOREACH_STATUS_OKAY(I2C_WS9652A_CHANNEL_DEFINE)
