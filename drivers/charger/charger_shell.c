/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/shell/shell.h>

struct charger_shell_property {
	const char *name;
	charger_prop_t property;
};

static const struct charger_shell_property properties[] = {
	{"status", CHARGER_PROP_STATUS},
	{"online", CHARGER_PROP_ONLINE},
	{"charge_type", CHARGER_PROP_CHARGE_TYPE},
	{"charge_current_ua", CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA},
	{"precharge_current_ua", CHARGER_PROP_PRECHARGE_CURRENT_UA},
	{"termination_current_ua", CHARGER_PROP_CHARGE_TERM_CURRENT_UA},
	{"input_current_ua", CHARGER_PROP_INPUT_REGULATION_CURRENT_UA},
	{"charge_voltage_uv", CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV},
};

static int get_charger(const struct shell *sh, const char *name, const struct device **charger)
{
	const struct device *dev = shell_device_get_binding_all(name);

	if (dev == NULL) {
		shell_error(sh, "%s: device not found", name);
		return -ENODEV;
	}
	if (!DEVICE_API_IS(charger, dev)) {
		shell_error(sh, "%s: not a charger device", name);
		return -ENODEV;
	}
	if (!dev->state->initialized) {
		shell_error(sh, "%s: not initialized", name);
		return -EAGAIN;
	}
	if (!device_is_ready(dev)) {
		int ret = -dev->state->init_res;

		shell_error(sh, "%s: initialization failed (%d)", name, ret);
		return ret;
	}
	*charger = dev;
	return 0;
}

static const struct charger_shell_property *get_property(const struct shell *sh, const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(properties); ++i) {
		if (strcmp(name, properties[i].name) == 0) {
			return &properties[i];
		}
	}
	shell_error(sh, "Unknown property: %s", name);
	return NULL;
}

static int print_property(const struct shell *sh, const struct device *dev,
			  const struct charger_shell_property *property)
{
	union charger_propval value;
	int ret = charger_get_prop(dev, property->property, &value);

	if (ret != 0) {
		shell_error(sh, "%d: %s: %s read failed", ret, dev->name, property->name);
		return ret;
	}
	static const char *const statuses[] = {
		"unknown", "charging", "discharging", "not charging", "full",
	};
	static const char *const online[] = {"offline", "fixed supply", "programmable supply"};
	static const char *const types[] = {
		"unknown", "none", "trickle", "fast", "standard", "adaptive", "long life", "bypass",
	};
	const char *text = NULL;
	uint32_t number = 0;

	switch (property->property) {
	case CHARGER_PROP_STATUS:
		number = value.status;
		text = number < ARRAY_SIZE(statuses) ? statuses[number] : "unknown";
		break;
	case CHARGER_PROP_ONLINE:
		number = value.online;
		text = number < ARRAY_SIZE(online) ? online[number] : "unknown";
		break;
	case CHARGER_PROP_CHARGE_TYPE:
		number = value.charge_type;
		text = number < ARRAY_SIZE(types) ? types[number] : "unknown";
		break;
	case CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA:
		number = value.const_charge_current_ua;
		break;
	case CHARGER_PROP_PRECHARGE_CURRENT_UA:
		number = value.precharge_current_ua;
		break;
	case CHARGER_PROP_CHARGE_TERM_CURRENT_UA:
		number = value.charge_term_current_ua;
		break;
	case CHARGER_PROP_INPUT_REGULATION_CURRENT_UA:
		number = value.input_current_regulation_current_ua;
		break;
	case CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV:
		number = value.const_charge_voltage_uv;
		break;
	default:
		return -ENOTSUP;
	}
	if (text != NULL) {
		shell_print(sh, "%s: %s", property->name, text);
	} else {
		shell_print(sh, "%s: %u", property->name, number);
	}
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	const struct device *dev;
	int ret = get_charger(sh, argv[1], &dev);

	if (ret != 0) {
		return ret;
	}
	for (size_t i = 0; i < 3; ++i) {
		ret = print_property(sh, dev, &properties[i]);

		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

static int cmd_get(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	const struct device *dev;
	int ret = get_charger(sh, argv[1], &dev);

	if (ret != 0) {
		return ret;
	}
	const struct charger_shell_property *property = get_property(sh, argv[2]);

	return property != NULL ? print_property(sh, dev, property) : -EINVAL;
}

static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	const struct device *dev;
	int ret = get_charger(sh, argv[1], &dev);

	if (ret != 0) {
		return ret;
	}
	const struct charger_shell_property *property = get_property(sh, argv[2]);

	if (property == NULL) {
		return -EINVAL;
	}
	unsigned long number = shell_strtoul(argv[3], 10, &ret);

	if (ret != 0 || argv[3][0] == '-' || number > UINT32_MAX) {
		shell_error(sh, "Invalid unsigned current: %s", argv[3]);
		return -EINVAL;
	}
	union charger_propval value = {0};

	switch (property->property) {
	case CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA:
		value.const_charge_current_ua = number;
		break;
	case CHARGER_PROP_PRECHARGE_CURRENT_UA:
		value.precharge_current_ua = number;
		break;
	case CHARGER_PROP_CHARGE_TERM_CURRENT_UA:
		value.charge_term_current_ua = number;
		break;
	case CHARGER_PROP_INPUT_REGULATION_CURRENT_UA:
		value.input_current_regulation_current_ua = number;
		break;
	default:
		shell_error(sh, "%s: read-only property", property->name);
		return -ENOTSUP;
	}
	ret = charger_set_prop(dev, property->property, &value);
	if (ret != 0) {
		shell_error(sh, "%d: %s: %s write failed", ret, dev->name, property->name);
	}
	return ret;
}

static int cmd_enable(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	const struct device *dev;
	int ret = get_charger(sh, argv[1], &dev);

	if (ret != 0) {
		return ret;
	}
	bool enable = strcmp(argv[2], "on") == 0;

	if (!enable && strcmp(argv[2], "off") != 0) {
		shell_error(sh, "Expected on or off");
		return -EINVAL;
	}
	ret = charger_charge_enable(dev, enable);

	if (ret != 0) {
		shell_error(sh, "%d: %s: charge %s failed", ret, dev->name, argv[2]);
	}
	return ret;
}

static bool is_charger(const struct device *dev)
{
	return DEVICE_API_IS(charger, dev);
}

static void device_name_get(size_t idx, struct shell_static_entry *entry)
{
	const struct device *dev = shell_device_filter(idx, is_charger);

	*entry = (struct shell_static_entry){.syntax = dev != NULL ? dev->name : NULL};
}

SHELL_DYNAMIC_CMD_CREATE(charger_devices, device_name_get);
SHELL_STATIC_SUBCMD_SET_CREATE(charger_commands,
	SHELL_CMD_ARG(status, &charger_devices, "Read status, online and charge type: <device>",
		      cmd_status, 2, 0),
	SHELL_CMD_ARG(get, &charger_devices,
		      "Read <device> <property>: status, online, charge_type, charge_current_ua, "
		      "precharge_current_ua, termination_current_ua, input_current_ua, charge_voltage_uv",
		      cmd_get, 3, 0),
	SHELL_CMD_ARG(set, &charger_devices, "Set current: <device> <property> <microamps>",
		      cmd_set, 4, 0),
	SHELL_CMD_ARG(enable, &charger_devices, "Control charging: <device> <on|off>",
		      cmd_enable, 3, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(charger, &charger_commands, "Charger commands", NULL);
