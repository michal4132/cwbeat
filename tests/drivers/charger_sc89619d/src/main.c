/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/irq_offload.h>
#include <zephyr/ztest.h>
#if defined(CONFIG_CHARGER_SHELL)
#include <zephyr/shell/shell_dummy.h>
#endif

/* Exercise the production driver through real charger, I2C and GPIO APIs. */
enum event_type { READ, WRITE, CE_HIGH, CE_LOW, CONFIGURE };
struct event {
	enum event_type type;
	uint8_t reg;
	uint8_t value;
};
static struct event events[64];
static size_t event_count;

static void record(enum event_type type, uint8_t reg, uint8_t value)
{
	if (event_count < ARRAY_SIZE(events)) {
		events[event_count++] = (struct event){type, reg, value};
	}
}

struct bus_data {
	uint8_t regs[256];
	int fail_read_reg;
	int fail_write_reg;
	int error;
	unsigned int reads;
	unsigned int writes;
};

static void reset_bus(struct bus_data *data)
{
	memset(data, 0, sizeof(*data));
	data->fail_read_reg = -1;
	data->fail_write_reg = -1;
	data->error = -EIO;
	data->regs[0x00] = 0xe4;
	data->regs[0x01] = 0xc5;
	data->regs[0x02] = 0xc1;
	data->regs[0x03] = 0xa7;
	data->regs[0x04] = 0x59;
	data->regs[0x05] = 0x8e;
	data->regs[0x0b] = 0x17; /* PN=2, unrelated version/reserved bits set. */
	data->regs[0x0d] = 0xe4;
}

static int bus_transfer(const struct device *dev, struct i2c_msg *msgs, uint8_t count,
			uint16_t address)
{
	struct bus_data *data = dev->data;
	uint8_t reg;

	if (address != 0x6a || msgs == NULL || count == 0 || msgs[0].len == 0) {
		return -EINVAL;
	}
	reg = msgs[0].buf[0];
	if (count == 2 && msgs[0].len == 1 && (msgs[0].flags & I2C_MSG_READ) == 0 &&
	    msgs[1].len == 1 && (msgs[1].flags & I2C_MSG_READ) != 0) {
		record(READ, reg, 0);
		data->reads++;
		if (data->fail_read_reg == reg) {
			return data->error;
		}
		msgs[1].buf[0] = data->regs[reg];
		return 0;
	}
	if (count == 1 && msgs[0].len == 2 && (msgs[0].flags & I2C_MSG_READ) == 0) {
		record(WRITE, reg, msgs[0].buf[1]);
		data->writes++;
		if (data->fail_write_reg == reg) {
			return data->error;
		}
		data->regs[reg] = msgs[0].buf[1];
		return 0;
	}
	return -ENOTSUP;
}

static int bus_configure(const struct device *dev, uint32_t config)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(config);
	return 0;
}

static int bus_init(const struct device *dev)
{
	reset_bus(dev->data);
	return 0;
}

static DEVICE_API(i2c, bus_api) = {
	.configure = bus_configure,
	.transfer = bus_transfer,
};

#define DT_DRV_COMPAT vnd_sc89619d_test_i2c
#define BUS_DEFINE(inst)                                                                         \
	static struct bus_data bus_data_##inst;                                                  \
	DEVICE_DT_INST_DEFINE(inst, bus_init, NULL, &bus_data_##inst, NULL,                       \
			      POST_KERNEL, CONFIG_I2C_INIT_PRIORITY, &bus_api);
DT_INST_FOREACH_STATUS_OKAY(BUS_DEFINE)
#undef DT_DRV_COMPAT

struct test_gpio_data {
	struct gpio_driver_data common;
	gpio_flags_t configured_flags;
	gpio_port_value_t output;
	unsigned int configurations;
	unsigned int sets;
	unsigned int fail_set;
	int configure_error;
};

static int gpio_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	struct test_gpio_data *data = dev->data;

	record(CONFIGURE, pin, 0);
	data->configurations++;
	data->configured_flags = flags;
	if (data->configure_error == 0) {
		if (flags & GPIO_OUTPUT_INIT_HIGH) {
			data->output |= BIT(pin);
		} else if (flags & GPIO_OUTPUT_INIT_LOW) {
			data->output &= ~BIT(pin);
		}
	}
	return data->configure_error;
}

static int gpio_set(const struct device *dev, gpio_port_pins_t pins, bool high)
{
	struct test_gpio_data *data = dev->data;

	record(high ? CE_HIGH : CE_LOW, pins, 0);
	data->sets++;
	if (data->fail_set == data->sets) {
		return -EACCES;
	}
	if (high) {
		data->output |= pins;
	} else {
		data->output &= ~pins;
	}
	return 0;
}

static int gpio_set_bits(const struct device *dev, gpio_port_pins_t pins)
{
	return gpio_set(dev, pins, true);
}

static int gpio_clear_bits(const struct device *dev, gpio_port_pins_t pins)
{
	return gpio_set(dev, pins, false);
}

static DEVICE_API(gpio, gpio_api) = {
	.pin_configure = gpio_configure,
	.port_set_bits_raw = gpio_set_bits,
	.port_clear_bits_raw = gpio_clear_bits,
};
static struct test_gpio_data gpio_data;
static const struct gpio_driver_config gpio_config = {
	.port_pin_mask = BIT_MASK(4),
};
DEVICE_DT_DEFINE(DT_NODELABEL(test_gpio), NULL, NULL, &gpio_data, &gpio_config,
		 PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY, &gpio_api);

#define DEV(name) DEVICE_DT_GET(DT_NODELABEL(name))
#define BUS(name) ((struct bus_data *)DEV(name)->data)
static const struct device *const charger = DEV(charger);
static bool preserved_init;
static int early_results[3];

static int early_access(void)
{
	union charger_propval val = {.const_charge_current_ua = 20000};

	early_results[0] = charger_get_prop(charger, CHARGER_PROP_STATUS, &val);
	early_results[1] = charger_set_prop(charger, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA,
					    &val);
	early_results[2] = charger_charge_enable(charger, true);
	return 0;
}
SYS_INIT(early_access, PRE_KERNEL_2, 99);

static void *setup(void)
{
	uint8_t inherited[256];

	memcpy(inherited, BUS(main_bus)->regs, sizeof(inherited));
	gpio_data.output = 0;
	zassert_ok(device_init(charger));
	preserved_init = BUS(main_bus)->reads == 1 && BUS(main_bus)->writes == 0 &&
		memcmp(inherited, BUS(main_bus)->regs, sizeof(inherited)) == 0 &&
		gpio_data.output == 0 && gpio_data.sets == 0 &&
		(gpio_data.configured_flags & GPIO_OUTPUT) != 0 &&
		(gpio_data.configured_flags & (GPIO_OUTPUT_INIT_HIGH | GPIO_OUTPUT_INIT_LOW)) == 0;
	zassert_ok(device_init(DEV(noce)));
	return NULL;
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	reset_bus(BUS(main_bus));
	reset_bus(BUS(noce_bus));
	event_count = 0;
	gpio_data.output = 0;
	gpio_data.sets = 0;
	gpio_data.configurations = 0;
	gpio_data.fail_set = 0;
	gpio_data.configure_error = 0;
}

ZTEST(sc89619d, test_init_preserves_inherited_policy_and_ce_latch)
{
	zassert_true(device_is_ready(charger));
	zassert_true(preserved_init);
}

ZTEST(sc89619d, test_init_errors_do_not_touch_ce)
{
	BUS(badid_bus)->regs[0x0b] = 0x08;
	zassert_equal(device_init(DEV(badid)), -ENODEV);
	zassert_equal(BUS(badid_bus)->writes, 0);
	BUS(init_error_bus)->fail_read_reg = 0x0b;
	BUS(init_error_bus)->error = -EBUSY;
	zassert_equal(device_init(DEV(init_error)), -EBUSY);
	zassert_equal(BUS(init_error_bus)->writes, 0);
	zassert_equal(device_init(DEV(invalid)), -EINVAL);
	zassert_equal(BUS(invalid_bus)->reads, 0);
	zassert_equal(gpio_data.configurations, 0);
	zassert_equal(gpio_data.sets, 0);
	zassert_false(device_is_ready(DEV(badid)));
	zassert_false(device_is_ready(DEV(init_error)));
}

ZTEST(sc89619d, test_init_gpio_error_propagates)
{
	gpio_data.configure_error = -EACCES;
	zassert_equal(device_init(DEV(gpio_error)), -EACCES);
	zassert_equal(BUS(gpio_error_bus)->reads, 1);
	zassert_equal(BUS(gpio_error_bus)->writes, 0);
	zassert_equal(gpio_data.sets, 0);
}

ZTEST(sc89619d, test_explicit_init_currents_only)
{
	struct bus_data *bus = BUS(configured_bus);
	uint8_t inherited[256];

	memcpy(inherited, bus->regs, sizeof(inherited));
	zassert_ok(device_init(DEV(configured)));
	inherited[0x00] = 0xe9;
	inherited[0x02] = 0xd5;
	inherited[0x03] = 0x12;
	zassert_mem_equal(inherited, bus->regs, sizeof(inherited));
	zassert_equal(bus->writes, 3);
	zassert_equal(gpio_data.sets, 0);
}

/* Independent expected writes from original TOP 0x53494, SHA-256
 * 0d7b8f82fa37732367a77f46af607297abfd06197f502d5b23e420a96fcd8750.
 * Fixtures above include the actual Go 4 devicetree configuration.
 */
static const uint8_t oem_writes[][2] = {
	{0x00, 0x09}, {0x02, 0x95}, {0x03, 0x11}, {0x04, 0x79},
	{0x05, 0x8e}, {0x0c, 0x36}, {0x0e, 0x32},
};

static void prepare_oem_bus(struct bus_data *bus)
{
	reset_bus(bus);
	bus->regs[0x00] = 0x04;
	bus->regs[0x01] = 0x0a; /* Exercise an initially disabled software gate. */
	bus->regs[0x02] = 0xa2;
	bus->regs[0x03] = 0x22;
	bus->regs[0x04] = 0x58;
	bus->regs[0x05] = 0x9f;
	bus->regs[0x06] = 0x66;
	bus->regs[0x07] = 0x54;
	bus->regs[0x0c] = 0x35;
	bus->regs[0x0e] = 0xb2;
}

ZTEST(sc89619d, test_go4_oem_configuration_and_enable_order)
{
	struct bus_data *bus = BUS(oem_bus);
	uint8_t expected[256];

	prepare_oem_bus(bus);
	memcpy(expected, bus->regs, sizeof(expected));
	for (size_t i = 0; i < ARRAY_SIZE(oem_writes); ++i) {
		expected[oem_writes[i][0]] = oem_writes[i][1];
	}
	expected[0x01] |= BIT(4);
	zassert_ok(device_init(DEV(oem)));
	zassert_mem_equal(expected, bus->regs, sizeof(expected));
	zassert_true(gpio_data.configured_flags & GPIO_OUTPUT_INIT_HIGH);
	zassert_equal(gpio_data.output & BIT(3), 0);
	size_t writes = 0;

	for (size_t i = 0; i < event_count; ++i) {
		if (events[i].type == WRITE) {
			if (writes < ARRAY_SIZE(oem_writes)) {
				zassert_equal(events[i].reg, oem_writes[writes][0]);
				zassert_equal(events[i].value, oem_writes[writes][1]);
			} else {
				zassert_equal(events[i].reg, 0x01);
				zassert_equal(events[i].value, expected[0x01]);
			}
			++writes;
		}
		if (events[i].type == CE_LOW) {
			zassert_equal(writes, ARRAY_SIZE(oem_writes) + 1);
		}
	}
	zassert_equal(writes, ARRAY_SIZE(oem_writes) + 1);
	zassert_equal(events[event_count - 1].type, CE_LOW);
	const struct { charger_prop_t prop; uint32_t value; } properties[] = {
		{CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, 420000},
		{CHARGER_PROP_PRECHARGE_CURRENT_UA, 40000},
		{CHARGER_PROP_CHARGE_TERM_CURRENT_UA, 40000},
		{CHARGER_PROP_INPUT_REGULATION_CURRENT_UA, 1000000},
		/* The existing +24 mV trim must survive configuration. */
		{CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV, 4352000},
	};
	for (size_t i = 0; i < ARRAY_SIZE(properties); ++i) {
		union charger_propval value;

		zassert_ok(charger_get_prop(DEV(oem), properties[i].prop, &value));
		zassert_equal(value.custom_uint, properties[i].value);
	}
}

ZTEST(sc89619d, test_go4_oem_without_ce_preserves_unconfigured_bits)
{
	struct bus_data *bus = BUS(oem_noce_bus);
	uint8_t expected[256];

	prepare_oem_bus(bus);
	bus->regs[0x01] = 0xd5; /* Enabled, with a self-clearing watchdog command. */
	bus->regs[0x02] |= BIT(6); /* Reserved. */
	bus->regs[0x05] |= BIT(6); /* Reserved. */
	bus->regs[0x0e] |= BIT(6); /* Read-only INPUT_DET_DONE. */
	memcpy(expected, bus->regs, sizeof(expected));
	for (size_t i = 0; i < ARRAY_SIZE(oem_writes); ++i) {
		uint8_t reg = oem_writes[i][0];
		uint8_t preserved = (reg == 2 || reg == 5 || reg == 0x0e) ? BIT(6) : 0;

		expected[reg] = (expected[reg] & preserved) | oem_writes[i][1];
	}
	expected[0x01] &= ~BIT(6);
	zassert_ok(device_init(DEV(oem_noce)));
	zassert_mem_equal(expected, bus->regs, sizeof(expected));
	/* Close CHG_CFG before the first policy write, restore it last. */
	zassert_equal(events[2].type, WRITE);
	zassert_equal(events[2].reg, 0x01);
	zassert_equal(events[2].value, 0x85);
	zassert_equal(events[event_count - 1].type, WRITE);
	zassert_equal(events[event_count - 1].reg, 0x01);
	zassert_equal(events[event_count - 1].value, 0x95);
	zassert_equal(gpio_data.configurations, 0);
	zassert_equal(gpio_data.sets, 0);
}

ZTEST(sc89619d, test_go4_oem_configuration_errors_leave_ce_disabled)
{
	const struct device *const devs[] = {DEV(oem_read_error), DEV(oem_write_error),
		DEV(oem_enable_error)};
	struct bus_data *const buses[] = {BUS(oem_read_error_bus), BUS(oem_write_error_bus),
		BUS(oem_enable_error_bus)};

	for (size_t i = 0; i < ARRAY_SIZE(devs); ++i) {
		struct bus_data *bus = buses[i];

		prepare_oem_bus(bus);
		event_count = 0;
		gpio_data.output = 0;
		if (i == 1) {
			bus->fail_write_reg = 0x04;
		} else {
			bus->fail_read_reg = i == 0 ? 0x04 : 0x01;
		}
		bus->error = -ETIMEDOUT;
		zassert_equal(device_init(devs[i]), -ETIMEDOUT);
		zassert_false(device_is_ready(devs[i]));
		zassert_equal(gpio_data.output & BIT(3), BIT(3));
		for (size_t j = 0; j < event_count; ++j) {
			zassert_not_equal(events[j].type, CE_LOW);
		}
		if (i < 2) {
			zassert_equal(bus->regs[0x05], 0x9f);
			zassert_equal(bus->regs[0x0c], 0x35);
			zassert_equal(bus->regs[0x0e], 0xb2);
		}
	}
}

ZTEST(sc89619d, test_go4_oem_preflight_failure_preserves_charge_state)
{
	BUS(oem_identity_error_bus)->fail_read_reg = 0x0b;
	BUS(oem_identity_error_bus)->error = -EBUSY;
	zassert_equal(device_init(DEV(oem_identity_error)), -EBUSY);
	zassert_equal(BUS(oem_identity_error_bus)->writes, 0);
	zassert_equal(device_init(DEV(invalid_voltage)), -EINVAL);
	zassert_equal(BUS(invalid_voltage_bus)->reads, 0);
	zassert_equal(BUS(invalid_voltage_bus)->writes, 0);
	zassert_equal(gpio_data.configurations, 0);
	zassert_equal(gpio_data.sets, 0);
	zassert_equal(gpio_data.output, 0);
}

static const struct {
	charger_prop_t prop;
	uint8_t reg;
	uint8_t mask;
	uint8_t shift;
	uint32_t min;
	uint32_t max;
	uint32_t step;
} current_cases[] = {
	{CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, 2, 0x3f, 0, 0, 1260000, 20000},
	{CHARGER_PROP_PRECHARGE_CURRENT_UA, 3, 0xf0, 4, 20000, 320000, 20000},
	{CHARGER_PROP_CHARGE_TERM_CURRENT_UA, 3, 0x0f, 0, 20000, 320000, 20000},
	{CHARGER_PROP_INPUT_REGULATION_CURRENT_UA, 0, 0x1f, 0, 100000, 3200000, 100000},
};

ZTEST(sc89619d, test_current_limits_rounding_and_neighbor_preservation)
{
	struct bus_data *bus = BUS(main_bus);

	for (size_t i = 0; i < ARRAY_SIZE(current_cases); i++) {
		const uint32_t targets[] = {current_cases[i].min, current_cases[i].max,
			current_cases[i].min + current_cases[i].step + 1};

		for (size_t j = 0; j < ARRAY_SIZE(targets); j++) {
			union charger_propval val = {.custom_uint = targets[j]};
			uint8_t old = 0xa5;
			uint32_t quantized = current_cases[i].min +
				(targets[j] - current_cases[i].min) / current_cases[i].step *
				current_cases[i].step;

			bus->regs[current_cases[i].reg] = old;
			zassert_ok(charger_set_prop(charger, current_cases[i].prop, &val));
			zassert_equal(bus->regs[current_cases[i].reg] & ~current_cases[i].mask,
				      old & ~current_cases[i].mask);
			zassert_ok(charger_get_prop(charger, current_cases[i].prop, &val));
			zassert_equal(val.custom_uint, quantized);
		}
		const uint32_t invalid[] = {current_cases[i].max + 1, UINT32_MAX,
			current_cases[i].min == 0 ? UINT32_MAX : current_cases[i].min - 1};

		for (size_t j = 0; j < ARRAY_SIZE(invalid); j++) {
			union charger_propval val = {.custom_uint = invalid[j]};
			unsigned int reads = bus->reads;
			unsigned int writes = bus->writes;

			zassert_equal(charger_set_prop(charger, current_cases[i].prop, &val), -EINVAL);
			zassert_equal(bus->reads, reads);
			zassert_equal(bus->writes, writes);
		}
	}
}

ZTEST(sc89619d, test_status_type_and_online)
{
	const enum charger_status statuses[] = {CHARGER_STATUS_NOT_CHARGING,
		CHARGER_STATUS_CHARGING, CHARGER_STATUS_CHARGING, CHARGER_STATUS_FULL};
	const enum charger_charge_type types[] = {CHARGER_CHARGE_TYPE_NONE,
		CHARGER_CHARGE_TYPE_TRICKLE, CHARGER_CHARGE_TYPE_FAST, CHARGER_CHARGE_TYPE_NONE};
	union charger_propval val;

	for (size_t i = 0; i < ARRAY_SIZE(statuses); i++) {
		BUS(main_bus)->regs[0x08] = 0xe7 | (i << 3);
		zassert_ok(charger_get_prop(charger, CHARGER_PROP_STATUS, &val));
		zassert_equal(val.status, statuses[i]);
		zassert_ok(charger_get_prop(charger, CHARGER_PROP_CHARGE_TYPE, &val));
		zassert_equal(val.charge_type, types[i]);
	}
	BUS(main_bus)->regs[0x0a] = 0x7f;
	zassert_ok(charger_get_prop(charger, CHARGER_PROP_ONLINE, &val));
	zassert_equal(val.online, CHARGER_ONLINE_OFFLINE);
	BUS(main_bus)->regs[0x0a] = 0x80;
	zassert_ok(charger_get_prop(charger, CHARGER_PROP_ONLINE, &val));
	zassert_equal(val.online, CHARGER_ONLINE_FIXED);
}

ZTEST(sc89619d, test_voltage_includes_fine_trim_and_preserves_output_on_error)
{
	struct bus_data *bus = BUS(main_bus);
	union charger_propval val;
	union charger_propval saved;

	for (uint32_t coarse = 0; coarse < 32; coarse++) {
		for (uint32_t fine = 0; fine < 4; fine++) {
			bus->regs[0x04] = (coarse << 3) | 7;
			bus->regs[0x0d] = (fine << 6) | 0x3f;
			zassert_ok(charger_get_prop(charger,
				CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV, &val));
			zassert_equal(val.const_charge_voltage_uv,
				      3848000 + coarse * 32000 + fine * 8000);
		}
	}
	memset(&val, 0xa5, sizeof(val));
	saved = val;
	bus->fail_read_reg = 0x0d;
	zassert_equal(charger_get_prop(charger, CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV, &val),
		      -EIO);
	zassert_mem_equal(&val, &saved, sizeof(val));
	bus->fail_read_reg = -1;
	zassert_equal(charger_set_prop(charger, CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV, &val),
		      -ENOTSUP);
	zassert_equal(bus->writes, 0);
}

ZTEST(sc89619d, test_get_errors_leave_entire_output_unchanged)
{
	const charger_prop_t props[] = {CHARGER_PROP_ONLINE, CHARGER_PROP_STATUS,
		CHARGER_PROP_CHARGE_TYPE, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA,
		CHARGER_PROP_PRECHARGE_CURRENT_UA, CHARGER_PROP_CHARGE_TERM_CURRENT_UA,
		CHARGER_PROP_INPUT_REGULATION_CURRENT_UA, CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV};
	const uint8_t registers[] = {0x0a, 8, 8, 2, 3, 3, 0, 4};
	union charger_propval val;
	union charger_propval saved;

	memset(&saved, 0x5a, sizeof(saved));
	for (size_t i = 0; i < ARRAY_SIZE(props); i++) {
		val = saved;
		BUS(main_bus)->fail_read_reg = registers[i];
		BUS(main_bus)->error = -EBUSY;
		zassert_equal(charger_get_prop(charger, props[i], &val), -EBUSY);
		zassert_mem_equal(&val, &saved, sizeof(val));
	}
	val = saved;
	zassert_equal(charger_get_prop(charger, CHARGER_PROP_HEALTH, &val), -ENOTSUP);
	zassert_mem_equal(&val, &saved, sizeof(val));
}

ZTEST(sc89619d, test_set_errors_and_noop_do_not_write)
{
	struct bus_data *bus = BUS(main_bus);
	union charger_propval val = {.const_charge_current_ua = 420000};

	bus->fail_read_reg = 2;
	zassert_equal(charger_set_prop(charger, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &val),
		      -EIO);
	zassert_equal(bus->writes, 0);
	bus->fail_read_reg = -1;
	bus->fail_write_reg = 2;
	zassert_equal(charger_set_prop(charger, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &val),
		      -EIO);
	zassert_equal(bus->regs[2], 0xc1);
	bus->fail_write_reg = -1;
	bus->writes = 0;
	val.const_charge_current_ua = 20000;
	zassert_ok(charger_set_prop(charger, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &val));
	zassert_equal(bus->writes, 0);
}

ZTEST(sc89619d, test_enable_disable_order_and_self_clearing_bit)
{
	struct bus_data *bus = BUS(main_bus);

	zassert_ok(charger_charge_enable(charger, true));
	zassert_equal(event_count, 4);
	zassert_equal(events[0].type, READ);
	zassert_equal(events[0].reg, 1);
	zassert_equal(events[1].type, CE_HIGH);
	zassert_equal(events[2].type, WRITE);
	zassert_equal(events[2].value, 0x95); /* Preserve neighbors, do not replay WD_RST. */
	zassert_equal(events[3].type, CE_LOW);
	zassert_equal(gpio_data.output & BIT(0), 0);
	bus->regs[1] = 0xd5;
	event_count = 0;
	zassert_ok(charger_charge_enable(charger, false));
	zassert_equal(event_count, 3);
	zassert_equal(events[0].type, CE_HIGH);
	zassert_equal(events[1].type, READ);
	zassert_equal(events[2].type, WRITE);
	zassert_equal(events[2].value, 0x85);
	zassert_equal(gpio_data.output & BIT(0), BIT(0));
}

ZTEST(sc89619d, test_enable_error_ordering)
{
	struct bus_data *bus = BUS(main_bus);

	bus->fail_read_reg = 1;
	bus->error = -EBUSY;
	zassert_equal(charger_charge_enable(charger, true), -EBUSY);
	zassert_equal(gpio_data.sets, 0);
	zassert_equal(gpio_data.output, 0); /* Refused initial read preserves /CE. */
	bus->fail_read_reg = -1;
	bus->fail_write_reg = 1;
	zassert_equal(charger_charge_enable(charger, true), -EBUSY);
	zassert_equal(gpio_data.sets, 1);
	zassert_equal(gpio_data.output & BIT(0), BIT(0));
	bus->fail_write_reg = -1;
	gpio_data.sets = 0;
	gpio_data.fail_set = 1;
	unsigned int writes = bus->writes;

	zassert_equal(charger_charge_enable(charger, true), -EACCES);
	zassert_equal(bus->writes, writes);
	gpio_data.sets = 0;
	gpio_data.fail_set = 2;
	zassert_equal(charger_charge_enable(charger, true), -EACCES);
	zassert_equal(bus->regs[1], 0x95);
	zassert_equal(gpio_data.output & BIT(0), BIT(0));
}

ZTEST(sc89619d, test_disable_attempts_both_gates_and_propagates_errors)
{
	struct bus_data *bus = BUS(main_bus);

	bus->regs[1] = 0x15;
	bus->fail_read_reg = 1;
	bus->error = -EBUSY;
	zassert_equal(charger_charge_enable(charger, false), -EBUSY);
	zassert_equal(gpio_data.output & BIT(0), BIT(0));
	bus->fail_read_reg = -1;
	gpio_data.output = 0;
	gpio_data.sets = 0;
	gpio_data.fail_set = 1;
	zassert_equal(charger_charge_enable(charger, false), -EACCES);
	zassert_equal(bus->regs[1], 0x05);
}

ZTEST(sc89619d, test_enable_without_ce)
{
	zassert_ok(charger_charge_enable(DEV(noce), true));
	zassert_equal(BUS(noce_bus)->regs[1], 0x95);
	zassert_ok(charger_charge_enable(DEV(noce), false));
	zassert_equal(BUS(noce_bus)->regs[1], 0x85);
	zassert_equal(gpio_data.sets, 0);
}

static void isr_access(const void *arg)
{
	int *results = (int *)arg;
	union charger_propval val = {.const_charge_current_ua = 20000};

	results[0] = charger_get_prop(charger, CHARGER_PROP_STATUS, &val);
	results[1] = charger_set_prop(charger, CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA, &val);
	results[2] = charger_charge_enable(charger, true);
}

ZTEST(sc89619d, test_thread_context_required)
{
	int results[3] = {0};

	irq_offload(isr_access, results);
	for (size_t i = 0; i < ARRAY_SIZE(results); i++) {
		zassert_equal(results[i], -EWOULDBLOCK);
		zassert_equal(early_results[i], -EWOULDBLOCK);
	}
	zassert_equal(BUS(main_bus)->reads, 0);
	zassert_equal(BUS(main_bus)->writes, 0);
	zassert_equal(gpio_data.sets, 0);
}

#if defined(CONFIG_CHARGER_SHELL)
static const struct shell *test_shell(void)
{
	const struct shell *sh = shell_backend_dummy_get_ptr();

	for (unsigned i = 0; i < 50 && !shell_ready(sh); ++i) {
		k_msleep(1);
	}
	zassert_true(shell_ready(sh));
	shell_backend_dummy_clear_output(sh);
	return sh;
}

ZTEST(sc89619d, test_shell_status_and_property_are_read_only)
{
	const struct shell *sh = test_shell();

	BUS(main_bus)->regs[0x08] = 2 << 3;
	BUS(main_bus)->regs[0x0a] = BIT(7);
	zassert_ok(shell_execute_cmd(sh, "charger status test_charger"));
	zassert_ok(shell_execute_cmd(sh, "charger get test_charger charge_current_ua"));
	size_t length;
	const char *output = shell_backend_dummy_get_output(sh, &length);

	zassert_not_null(strstr(output, "status: charging"));
	zassert_not_null(strstr(output, "online: fixed supply"));
	zassert_not_null(strstr(output, "charge_type: fast"));
	zassert_not_null(strstr(output, "charge_current_ua: 20000"));
	zassert_equal(BUS(main_bus)->writes, 0);
	zassert_equal(gpio_data.sets, 0);
}

ZTEST(sc89619d, test_shell_current_and_enable_use_charger_api)
{
	const struct shell *sh = test_shell();

	zassert_ok(shell_execute_cmd(sh, "charger set test_charger charge_current_ua 420001"));
	zassert_equal(BUS(main_bus)->regs[0x02] & 0x3f, 21);
	zassert_ok(shell_execute_cmd(sh, "charger enable test_charger on"));
	zassert_true(BUS(main_bus)->regs[0x01] & BIT(4));
	zassert_equal(gpio_data.output & BIT(0), 0);
	zassert_ok(shell_execute_cmd(sh, "charger enable test_charger off"));
	zassert_false(BUS(main_bus)->regs[0x01] & BIT(4));
	zassert_equal(gpio_data.output & BIT(0), BIT(0));
}

ZTEST(sc89619d, test_shell_invalid_input_performs_no_io)
{
	const struct shell *sh = test_shell();
	static const struct { const char *command; int error; } cases[] = {
		{"charger status missing", -ENODEV},
		{"charger status main-bus", -ENODEV},
		{"charger status", -EINVAL},
		{"charger get test_charger missing", -EINVAL},
		{"charger set test_charger status 1", -ENOTSUP},
		{"charger set test_charger charge_current_ua -1", -EINVAL},
		{"charger set test_charger charge_current_ua 4294967296", -EINVAL},
		{"charger set test_charger charge_current_ua 10mA", -EINVAL},
		{"charger set test_charger charge_current_ua 1260001", -EINVAL},
		{"charger enable test_charger true", -EINVAL},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); ++i) {
		zassert_equal(shell_execute_cmd(sh, cases[i].command), cases[i].error,
			      "%s", cases[i].command);
	}
	zassert_equal(BUS(main_bus)->reads, 0);
	zassert_equal(BUS(main_bus)->writes, 0);
	zassert_equal(gpio_data.sets, 0);
}

ZTEST(sc89619d, test_shell_preserves_transport_refusal)
{
	const struct shell *sh = test_shell();

	BUS(main_bus)->fail_read_reg = 0x08;
	BUS(main_bus)->error = -EBUSY;
	zassert_equal(shell_execute_cmd(sh, "charger status test_charger"), -EBUSY);
	zassert_equal(BUS(main_bus)->reads, 1);
	zassert_equal(BUS(main_bus)->writes, 0);
	zassert_equal(gpio_data.sets, 0);
}

ZTEST(sc89619d, test_shell_reports_pending_and_failed_init_without_io)
{
	const struct shell *sh = test_shell();
	const struct device *dev = DEV(shell_charger);
	struct bus_data *bus = BUS(shell_bus);
	const char *const commands[] = {
		"charger status test_deferred_charger",
		"charger get test_deferred_charger status",
		"charger set test_deferred_charger charge_current_ua 420000",
		"charger enable test_deferred_charger on",
	};

	for (size_t phase = 0; phase < 2; ++phase) {
		for (size_t i = 0; i < ARRAY_SIZE(commands); ++i) {
			shell_backend_dummy_clear_output(sh);
			zassert_equal(shell_execute_cmd(sh, commands[i]), phase ? -EIO : -EAGAIN);
			size_t length;
			const char *output = shell_backend_dummy_get_output(sh, &length);

			zassert_not_null(strstr(output, phase ? "initialization failed (-5)" :
					       "not initialized"));
		}
		zassert_equal(bus->reads, 0);
		zassert_equal(bus->writes, 0);
		zassert_equal(gpio_data.configurations, 0);
		zassert_equal(gpio_data.sets, 0);
		if (phase == 0) {
			zassert_false(dev->state->initialized);
			bus->fail_read_reg = 0x0b;
			zassert_equal(device_init(dev), -EIO);
			zassert_false(device_is_ready(dev));
			bus->reads = 0;
		}
	}
}
#endif

ZTEST_SUITE(sc89619d, NULL, setup, before, NULL, NULL);
