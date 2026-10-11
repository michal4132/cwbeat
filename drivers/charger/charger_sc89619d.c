/* SPDX-License-Identifier: Apache-2.0 */

#define DT_DRV_COMPAT southchip_sc89619d

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/charger.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(charger_sc89619d, CONFIG_CHARGER_SC89619D_LOG_LEVEL);

#define SC89619D_REG_INPUT  0x00
#define SC89619D_REG_CTRL   0x01
#define SC89619D_REG_ICC    0x02
#define SC89619D_REG_ITC    0x03
#define SC89619D_REG_VBAT   0x04
#define SC89619D_REG_TIMER  0x05
#define SC89619D_REG_STATUS 0x08
#define SC89619D_REG_VBUS   0x0a
#define SC89619D_REG_ID     0x0b
#define SC89619D_REG_JEITA  0x0c
#define SC89619D_REG_TRIM   0x0d
#define SC89619D_REG_MISC   0x0e

#define SC89619D_EN_HIZ BIT(7)
#define SC89619D_STAT_PIN GENMASK(6, 5)
#define SC89619D_IINDPM GENMASK(4, 0)
#define SC89619D_CHG_CFG BIT(4)
#define SC89619D_WD_RST  BIT(6)
#define SC89619D_BOOST_LIM BIT(7)
#define SC89619D_ICC GENMASK(5, 0)
#define SC89619D_ITC GENMASK(7, 4)
#define SC89619D_ITERM GENMASK(3, 0)
#define SC89619D_VBAT_REG GENMASK(7, 3)
#define SC89619D_TOP_OFF GENMASK(2, 1)
#define SC89619D_VRECHG BIT(0)
#define SC89619D_EN_TERM BIT(7)
#define SC89619D_WATCHDOG GENMASK(5, 4)
#define SC89619D_SAFETY_TIMER GENMASK(3, 2)
#define SC89619D_TREG BIT(1)
#define SC89619D_COOL_ISET1 BIT(0)
#define SC89619D_COOL_ISET2 BIT(7)
#define SC89619D_WARM_VSET2 BIT(6)
#define SC89619D_WARM_ISET GENMASK(5, 4)
#define SC89619D_COOL_TEMP GENMASK(3, 2)
#define SC89619D_WARM_TEMP GENMASK(1, 0)
#define SC89619D_VTC BIT(7)
#define SC89619D_AUTO_DPDM BIT(5)
#define SC89619D_BUCK_FREQ BIT(4)
#define SC89619D_BOOST_FREQ BIT(3)
#define SC89619D_VSYSOVP GENMASK(2, 1)
#define SC89619D_NTC_DIS BIT(0)
#define SC89619D_CHRG_STAT GENMASK(4, 3)
#define SC89619D_VBUS_GD BIT(7)
#define SC89619D_PN_MASK GENMASK(6, 3)
#define SC89619D_PN FIELD_PREP(SC89619D_PN_MASK, 2)
#define SC89619D_VOLTAGE_MIN_UV 3848000U
#define SC89619D_VOLTAGE_MAX_UV 4840000U
#define SC89619D_VOLTAGE_STEP_UV 32000U
#define SC89619D_VOLTAGE_CONFIGURED BIT(4)

struct sc89619d_init_setting {
	uint8_t reg;
	uint8_t mask;
	uint8_t value;
};

struct sc89619d_config {
	struct i2c_dt_spec i2c;
	struct gpio_dt_spec ce;
	/* /INT wiring placeholder; interrupt handling is not implemented yet. */
	struct gpio_dt_spec irq;
	uint32_t charge_ua;
	uint32_t precharge_ua;
	uint32_t termination_ua;
	uint32_t input_ua;
	uint32_t voltage_uv;
	const struct sc89619d_init_setting *settings;
	size_t settings_count;
	bool enable_charging;
	uint8_t configured;
};

struct sc89619d_data {
	struct k_mutex lock;
};

struct sc89619d_current {
	uint8_t reg;
	uint8_t mask;
	uint8_t shift;
	uint32_t min;
	uint32_t max;
	uint32_t step;
};

static const struct sc89619d_current charge_current = {
	SC89619D_REG_ICC, GENMASK(5, 0), 0, 0, 1260000, 20000,
};
static const struct sc89619d_current precharge_current = {
	SC89619D_REG_ITC, GENMASK(7, 4), 4, 20000, 320000, 20000,
};
static const struct sc89619d_current termination_current = {
	SC89619D_REG_ITC, GENMASK(3, 0), 0, 20000, 320000, 20000,
};
static const struct sc89619d_current input_current = {
	SC89619D_REG_INPUT, GENMASK(4, 0), 0, 100000, 3200000, 100000,
};

static int sc89619d_read(const struct device *dev, uint8_t reg, uint8_t *value)
{
	const struct sc89619d_config *config = dev->config;

	int ret = i2c_reg_read_byte_dt(&config->i2c, reg, value);

	if (ret != 0) {
		LOG_DBG("%d: %s: register 0x%02x read failed", ret, dev->name, reg);
	}
	return ret;
}

static int sc89619d_write_update(const struct device *dev, uint8_t reg, uint8_t old,
				uint8_t mask, uint8_t value)
{
	const struct sc89619d_config *config = dev->config;

	if ((old & mask) == (value & mask)) {
		return 0;
	}
	value = (old & ~mask) | (value & mask);
	/* Do not replay the self-clearing watchdog command during a control RMW. */
	if (reg == SC89619D_REG_CTRL) {
		value &= ~SC89619D_WD_RST;
	}
	int ret = i2c_reg_write_byte_dt(&config->i2c, reg, value);

	LOG_DBG("%d: %s: register 0x%02x write: 0x%02x", ret, dev->name, reg, value);
	return ret;
}

static int sc89619d_update(const struct device *dev, uint8_t reg, uint8_t mask,
			 uint8_t value)
{
	uint8_t old;
	int ret = sc89619d_read(dev, reg, &old);

	return ret != 0 ? ret : sc89619d_write_update(dev, reg, old, mask, value);
}

static int sc89619d_current_valid(const struct sc89619d_current *current, uint32_t ua)
{
	return ua < current->min || ua > current->max ? -EINVAL : 0;
}

static int sc89619d_set_current(const struct device *dev,
			       const struct sc89619d_current *current, uint32_t ua)
{
	int ret = sc89619d_current_valid(current, ua);

	if (ret != 0) {
		LOG_DBG("%d: %s: register 0x%02x: current %u uA out of range", ret,
			dev->name, current->reg, ua);
		return ret;
	}
	return sc89619d_update(dev, current->reg, current->mask,
			      ((ua - current->min) / current->step) << current->shift);
}

static int sc89619d_get_current(const struct device *dev,
			       const struct sc89619d_current *current, uint32_t *ua)
{
	uint8_t value;
	int ret = sc89619d_read(dev, current->reg, &value);

	if (ret == 0) {
		*ua = current->min + ((value & current->mask) >> current->shift) * current->step;
	}
	return ret;
}

static int sc89619d_get_property(const struct device *dev, charger_prop_t prop,
				union charger_propval *val)
{
	struct sc89619d_data *data = dev->data;
	union charger_propval result = {0};
	uint8_t value;
	uint8_t trim;
	int ret;

	if (val == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr() || k_is_pre_kernel()) {
		return -EWOULDBLOCK;
	}
	k_mutex_lock(&data->lock, K_FOREVER);
	switch (prop) {
	case CHARGER_PROP_ONLINE:
		ret = sc89619d_read(dev, SC89619D_REG_VBUS, &value);
		if (ret == 0) {
			result.online = (value & SC89619D_VBUS_GD) != 0 ?
				CHARGER_ONLINE_FIXED : CHARGER_ONLINE_OFFLINE;
		}
		break;
	case CHARGER_PROP_STATUS:
	case CHARGER_PROP_CHARGE_TYPE:
		ret = sc89619d_read(dev, SC89619D_REG_STATUS, &value);
		if (ret == 0) {
			value = FIELD_GET(SC89619D_CHRG_STAT, value);
			if (prop == CHARGER_PROP_STATUS) {
				result.status = value == 3 ? CHARGER_STATUS_FULL :
					value == 0 ? CHARGER_STATUS_NOT_CHARGING :
					CHARGER_STATUS_CHARGING;
			} else {
				result.charge_type = value == 1 ? CHARGER_CHARGE_TYPE_TRICKLE :
					value == 2 ? CHARGER_CHARGE_TYPE_FAST :
					CHARGER_CHARGE_TYPE_NONE;
			}
		}
		break;
	case CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA:
		ret = sc89619d_get_current(dev, &charge_current, &result.const_charge_current_ua);
		break;
	case CHARGER_PROP_PRECHARGE_CURRENT_UA:
		ret = sc89619d_get_current(dev, &precharge_current, &result.precharge_current_ua);
		break;
	case CHARGER_PROP_CHARGE_TERM_CURRENT_UA:
		ret = sc89619d_get_current(dev, &termination_current, &result.charge_term_current_ua);
		break;
	case CHARGER_PROP_INPUT_REGULATION_CURRENT_UA:
		ret = sc89619d_get_current(dev, &input_current,
					  &result.input_current_regulation_current_ua);
		break;
	case CHARGER_PROP_CONSTANT_CHARGE_VOLTAGE_UV:
		ret = sc89619d_read(dev, SC89619D_REG_VBAT, &value);
		if (ret == 0) {
			ret = sc89619d_read(dev, SC89619D_REG_TRIM, &trim);
		}
		if (ret == 0) {
			result.const_charge_voltage_uv = 3848000 + (value >> 3) * 32000 +
				(trim >> 6) * 8000;
		}
		break;
	default:
		ret = -ENOTSUP;
		break;
	}
	if (ret == 0) {
		*val = result;
	}
	k_mutex_unlock(&data->lock);
	if (ret != 0) {
		LOG_DBG("%d: %s: property %u read failed", ret, dev->name, prop);
	}
	return ret;
}

static void sc89619d_log_result(const struct device *dev, const char *operation, int ret)
{
	if (ret == 0) {
		LOG_DBG("%s: %s complete", dev->name, operation);
	} else if (ret == -EBUSY || ret == -EAGAIN || ret == -ENOTSUP || ret == -EINVAL) {
		LOG_DBG("%d: %s: %s refused", ret, dev->name, operation);
	} else {
		LOG_ERR("%d: %s: %s failed", ret, dev->name, operation);
	}
}

static int sc89619d_set_property(const struct device *dev, charger_prop_t prop,
				const union charger_propval *val)
{
	struct sc89619d_data *data = dev->data;
	int ret;

	if (val == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr() || k_is_pre_kernel()) {
		return -EWOULDBLOCK;
	}
	k_mutex_lock(&data->lock, K_FOREVER);
	switch (prop) {
	case CHARGER_PROP_CONSTANT_CHARGE_CURRENT_UA:
		ret = sc89619d_set_current(dev, &charge_current, val->const_charge_current_ua);
		break;
	case CHARGER_PROP_PRECHARGE_CURRENT_UA:
		ret = sc89619d_set_current(dev, &precharge_current, val->precharge_current_ua);
		break;
	case CHARGER_PROP_CHARGE_TERM_CURRENT_UA:
		ret = sc89619d_set_current(dev, &termination_current, val->charge_term_current_ua);
		break;
	case CHARGER_PROP_INPUT_REGULATION_CURRENT_UA:
		ret = sc89619d_set_current(dev, &input_current,
					  val->input_current_regulation_current_ua);
		break;
	default:
		ret = -ENOTSUP;
		break;
	}
	k_mutex_unlock(&data->lock);
	LOG_DBG("%d: %s: property %u write", ret, dev->name, prop);
	sc89619d_log_result(dev, "property write", ret);
	return ret;
}

static int sc89619d_charge_enable(const struct device *dev, bool enable)
{
	const struct sc89619d_config *config = dev->config;
	struct sc89619d_data *data = dev->data;
	uint8_t control = 0;
	int gpio_ret = 0;
	int ret;

	if (k_is_in_isr() || k_is_pre_kernel()) {
		return -EWOULDBLOCK;
	}
	k_mutex_lock(&data->lock, K_FOREVER);
	if (enable) {
		/* A controller may refuse access while another core owns the bus.
		 * Preserve the existing charge state if the initial read is refused.
		 */
		ret = sc89619d_read(dev, SC89619D_REG_CTRL, &control);
		if (ret != 0) {
			goto out;
		}
	}
	if (config->ce.port != NULL) {
		/* Keep /CE inactive until the software enable is successfully set. */
		gpio_ret = gpio_pin_set_dt(&config->ce, 0);
		if (gpio_ret != 0) {
			LOG_DBG("%d: %s: CEn deactivate failed", gpio_ret, dev->name);
		}
		if (gpio_ret != 0 && enable) {
			ret = gpio_ret;
			goto out;
		}
	}
	ret = enable ? sc89619d_write_update(dev, SC89619D_REG_CTRL, control,
					    SC89619D_CHG_CFG, SC89619D_CHG_CFG) :
		       sc89619d_update(dev, SC89619D_REG_CTRL, SC89619D_CHG_CFG, 0);
	if (gpio_ret != 0) {
		/* Still attempt software disable if deasserting /CE failed. */
		ret = gpio_ret;
	}
	if (ret == 0 && enable && config->ce.port != NULL) {
		ret = gpio_pin_set_dt(&config->ce, 1);
		if (ret != 0) {
			LOG_DBG("%d: %s: CEn activate failed", ret, dev->name);
		}
	}
out:
	k_mutex_unlock(&data->lock);
	sc89619d_log_result(dev, enable ? "charge enable" : "charge disable", ret);
	return ret;
}

static int sc89619d_init(const struct device *dev)
{
	const struct sc89619d_config *config = dev->config;
	struct sc89619d_data *data = dev->data;
	const struct sc89619d_current *const currents[] = {
		&charge_current, &precharge_current, &termination_current, &input_current,
	};
	const uint32_t values[] = {
		config->charge_ua, config->precharge_ua, config->termination_ua, config->input_ua,
	};
	uint8_t id;
	int ret;

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("%d: %s: I2C bus not ready", -ENODEV, dev->name);
		return -ENODEV;
	}
	if (config->ce.port != NULL && !gpio_is_ready_dt(&config->ce)) {
		LOG_ERR("%d: %s: CEn GPIO not ready", -ENODEV, dev->name);
		return -ENODEV;
	}
	if (config->ce.port != NULL && (config->ce.dt_flags & GPIO_ACTIVE_LOW) == 0) {
		LOG_ERR("%d: %s: CEn must be active low", -EINVAL, dev->name);
		return -EINVAL;
	}
	for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
		if ((config->configured & BIT(i)) != 0 &&
		    sc89619d_current_valid(currents[i], values[i]) != 0) {
			LOG_ERR("%d: %s: register 0x%02x: invalid initial current %u uA",
				-EINVAL, dev->name, currents[i]->reg, values[i]);
			return -EINVAL;
		}
	}
	if ((config->configured & SC89619D_VOLTAGE_CONFIGURED) != 0 &&
	    (config->voltage_uv < SC89619D_VOLTAGE_MIN_UV ||
	     config->voltage_uv > SC89619D_VOLTAGE_MAX_UV ||
	     (config->voltage_uv - SC89619D_VOLTAGE_MIN_UV) % SC89619D_VOLTAGE_STEP_UV != 0)) {
		LOG_ERR("%d: %s: invalid initial base voltage %u uV", -EINVAL, dev->name,
			config->voltage_uv);
		return -EINVAL;
	}
	ret = sc89619d_read(dev, SC89619D_REG_ID, &id);
	if (ret != 0) {
		LOG_ERR("%d: %s: identity read failed", ret, dev->name);
		return ret;
	}
	/* PN identifies this family; the board compatible selects the D pinout. */
	if ((id & SC89619D_PN_MASK) != SC89619D_PN) {
		LOG_ERR("%d: %s: unsupported identity 0x%02x", -ENODEV, dev->name, id);
		return -ENODEV;
	}
	k_mutex_init(&data->lock);
	if (config->ce.port != NULL) {
		ret = gpio_pin_configure_dt(&config->ce,
			config->enable_charging ? GPIO_OUTPUT_INACTIVE : GPIO_OUTPUT);
		if (ret != 0) {
			LOG_ERR("%d: %s: CEn configuration failed", ret, dev->name);
			return ret;
		}
	} else if (config->enable_charging) {
		/* Without /CE, hold the software gate closed while changing policy. */
		ret = sc89619d_update(dev, SC89619D_REG_CTRL, SC89619D_CHG_CFG, 0);
		if (ret != 0) {
			LOG_ERR("%d: %s: initial charge disable failed", ret, dev->name);
			return ret;
		}
	}
	for (size_t i = 0; i < config->settings_count; i++) {
		const struct sc89619d_init_setting *setting = &config->settings[i];

		if (setting->mask != 0) {
			ret = sc89619d_update(dev, setting->reg, setting->mask, setting->value);
			if (ret != 0) {
				LOG_ERR("%d: %s: register 0x%02x: initial settings failed",
					ret, dev->name, setting->reg);
				return ret;
			}
		}
	}
	if (config->enable_charging) {
		ret = sc89619d_charge_enable(dev, true);
		if (ret != 0) {
			LOG_ERR("%d: %s: initial charge enable failed", ret, dev->name);
			return ret;
		}
	}
	LOG_INF("%s: initialized: id=0x%02x", dev->name, id);
	return 0;
}

static DEVICE_API(charger, sc89619d_api) = {
	.get_property = sc89619d_get_property,
	.set_property = sc89619d_set_property,
	.charge_enable = sc89619d_charge_enable,
};

#define SC89619D_DT_MASK(inst, prop, mask) \
	(DT_INST_NODE_HAS_PROP(inst, prop) ? (mask) : 0)
#define SC89619D_DT_ENUM(inst, prop, mask) \
	FIELD_PREP(mask, DT_INST_ENUM_IDX_OR(inst, prop, 0))
#define SC89619D_DT_LINEAR(inst, prop, mask, base, step) \
	FIELD_PREP(mask, (DT_INST_PROP_OR(inst, prop, base) - (base)) / (step))
#define SC89619D_DT_COOL(inst) DT_INST_ENUM_IDX_OR(inst, jeita_cool_current_percent, 0)
#define SC89619D_DT_TIMER(inst) DT_INST_ENUM_IDX_OR(inst, charge_safety_timer_hours, 0)

/* Group fields into one update per register. Unspecified fields, reserved
 * bits and read-only status bits retain their previous values. The order
 * follows the Go 4 TOP initializer at 0x53494; values come from devicetree.
 */
#define SC89619D_SETTINGS(inst) {                                                          \
	{SC89619D_REG_INPUT,                                                               \
	 SC89619D_DT_MASK(inst, southchip_high_impedance_mode, SC89619D_EN_HIZ) |           \
	 SC89619D_DT_MASK(inst, southchip_stat_pin_mode, SC89619D_STAT_PIN) |               \
	 SC89619D_DT_MASK(inst, input_current_limit_microamp, SC89619D_IINDPM),             \
	 SC89619D_DT_ENUM(inst, southchip_high_impedance_mode, SC89619D_EN_HIZ) |           \
	 FIELD_PREP(SC89619D_STAT_PIN,                                                     \
		    DT_INST_ENUM_IDX_OR(inst, southchip_stat_pin_mode, 0) * 3) |             \
	 SC89619D_DT_LINEAR(inst, input_current_limit_microamp, SC89619D_IINDPM,            \
			   100000U, 100000U)},                                                  \
	{SC89619D_REG_ICC,                                                                 \
	 SC89619D_DT_MASK(inst, boost_current_limit_microamp, SC89619D_BOOST_LIM) |         \
	 SC89619D_DT_MASK(inst, constant_charge_current_max_microamp, SC89619D_ICC),        \
	 SC89619D_DT_ENUM(inst, boost_current_limit_microamp, SC89619D_BOOST_LIM) |         \
	 SC89619D_DT_LINEAR(inst, constant_charge_current_max_microamp, SC89619D_ICC,       \
			   0U, 20000U)},                                                       \
	{SC89619D_REG_ITC,                                                                 \
	 SC89619D_DT_MASK(inst, precharge_current_microamp, SC89619D_ITC) |                 \
	 SC89619D_DT_MASK(inst, charge_term_current_microamp, SC89619D_ITERM),              \
	 SC89619D_DT_LINEAR(inst, precharge_current_microamp, SC89619D_ITC, 20000U, 20000U) | \
	 SC89619D_DT_LINEAR(inst, charge_term_current_microamp, SC89619D_ITERM,             \
			   20000U, 20000U)},                                                   \
	{SC89619D_REG_VBAT,                                                                \
	 SC89619D_DT_MASK(inst, southchip_charge_voltage_base_microvolt, SC89619D_VBAT_REG) | \
	 SC89619D_DT_MASK(inst, top_off_time_minutes, SC89619D_TOP_OFF) |                  \
	 SC89619D_DT_MASK(inst, re_charge_threshold_microvolt, SC89619D_VRECHG),            \
	 SC89619D_DT_LINEAR(inst, southchip_charge_voltage_base_microvolt,                 \
			   SC89619D_VBAT_REG, SC89619D_VOLTAGE_MIN_UV, SC89619D_VOLTAGE_STEP_UV) | \
	 SC89619D_DT_ENUM(inst, top_off_time_minutes, SC89619D_TOP_OFF) |                  \
	 SC89619D_DT_ENUM(inst, re_charge_threshold_microvolt, SC89619D_VRECHG)},           \
	{SC89619D_REG_TIMER,                                                               \
	 SC89619D_DT_MASK(inst, charge_termination, SC89619D_EN_TERM) |                     \
	 SC89619D_DT_MASK(inst, watchdog_timeout_ms, SC89619D_WATCHDOG) |                   \
	 SC89619D_DT_MASK(inst, charge_safety_timer_hours, SC89619D_SAFETY_TIMER) |          \
	 SC89619D_DT_MASK(inst, thermal_regulation_celsius, SC89619D_TREG) |                \
	 SC89619D_DT_MASK(inst, jeita_cool_current_percent, SC89619D_COOL_ISET1),            \
	 SC89619D_DT_ENUM(inst, charge_termination, SC89619D_EN_TERM) |                     \
	 SC89619D_DT_ENUM(inst, watchdog_timeout_ms, SC89619D_WATCHDOG) |                   \
	 FIELD_PREP(SC89619D_SAFETY_TIMER,                                                 \
		    SC89619D_DT_TIMER(inst) ? SC89619D_DT_TIMER(inst) + 1 : 0) |              \
	 SC89619D_DT_ENUM(inst, thermal_regulation_celsius, SC89619D_TREG) |                \
	 FIELD_PREP(SC89619D_COOL_ISET1, SC89619D_DT_COOL(inst) & 1)},                       \
	{SC89619D_REG_JEITA,                                                               \
	 SC89619D_DT_MASK(inst, jeita_cool_current_percent, SC89619D_COOL_ISET2) |            \
	 SC89619D_DT_MASK(inst, southchip_jeita_warm_voltage_mode, SC89619D_WARM_VSET2) |    \
	 SC89619D_DT_MASK(inst, jeita_warm_current_percent, SC89619D_WARM_ISET) |            \
	 SC89619D_DT_MASK(inst, jeita_cool_temperature_millicelsius, SC89619D_COOL_TEMP) |   \
	 SC89619D_DT_MASK(inst, jeita_warm_temperature_millicelsius, SC89619D_WARM_TEMP),    \
	 FIELD_PREP(SC89619D_COOL_ISET2, SC89619D_DT_COOL(inst) >> 1) |                      \
	 SC89619D_DT_ENUM(inst, southchip_jeita_warm_voltage_mode, SC89619D_WARM_VSET2) |    \
	 SC89619D_DT_ENUM(inst, jeita_warm_current_percent, SC89619D_WARM_ISET) |            \
	 SC89619D_DT_ENUM(inst, jeita_cool_temperature_millicelsius, SC89619D_COOL_TEMP) |   \
	 SC89619D_DT_ENUM(inst, jeita_warm_temperature_millicelsius, SC89619D_WARM_TEMP)},    \
	{SC89619D_REG_MISC,                                                                \
	 SC89619D_DT_MASK(inst, precharge_voltage_threshold_microvolt, SC89619D_VTC) |       \
	 SC89619D_DT_MASK(inst, southchip_auto_dpdm, SC89619D_AUTO_DPDM) |                   \
	 SC89619D_DT_MASK(inst, buck_frequency_hz, SC89619D_BUCK_FREQ) |                    \
	 SC89619D_DT_MASK(inst, boost_frequency_hz, SC89619D_BOOST_FREQ) |                  \
	 SC89619D_DT_MASK(inst, system_overvoltage_percent, SC89619D_VSYSOVP) |             \
	 SC89619D_DT_MASK(inst, ntc_protection, SC89619D_NTC_DIS),                           \
	 SC89619D_DT_ENUM(inst, precharge_voltage_threshold_microvolt, SC89619D_VTC) |       \
	 SC89619D_DT_ENUM(inst, southchip_auto_dpdm, SC89619D_AUTO_DPDM) |                   \
	 SC89619D_DT_ENUM(inst, buck_frequency_hz, SC89619D_BUCK_FREQ) |                    \
	 SC89619D_DT_ENUM(inst, boost_frequency_hz, SC89619D_BOOST_FREQ) |                  \
	 SC89619D_DT_ENUM(inst, system_overvoltage_percent, SC89619D_VSYSOVP) |             \
	 SC89619D_DT_ENUM(inst, ntc_protection, SC89619D_NTC_DIS)},                          \
}

#define SC89619D_DEFINE(inst)                                                                    \
	BUILD_ASSERT(DT_INST_REG_ADDR(inst) == 0x6a, "SC89619D has fixed I2C address 0x6a");     \
	static struct sc89619d_data sc89619d_data_##inst;                                        \
	static const struct sc89619d_init_setting sc89619d_settings_##inst[] =                   \
		SC89619D_SETTINGS(inst);                                                        \
	static const struct sc89619d_config sc89619d_config_##inst = {                           \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                               \
		.ce = GPIO_DT_SPEC_INST_GET_OR(inst, ce_gpios, {0}),                             \
		.irq = GPIO_DT_SPEC_INST_GET_OR(inst, int_gpios, {0}),                           \
		.charge_ua = DT_INST_PROP_OR(inst, constant_charge_current_max_microamp, 0),     \
		.precharge_ua = DT_INST_PROP_OR(inst, precharge_current_microamp, 0),            \
		.termination_ua = DT_INST_PROP_OR(inst, charge_term_current_microamp, 0),        \
		.input_ua = DT_INST_PROP_OR(inst, input_current_limit_microamp, 0),              \
		.voltage_uv = DT_INST_PROP_OR(inst, southchip_charge_voltage_base_microvolt, 0), \
		.settings = sc89619d_settings_##inst,                                          \
		.settings_count = ARRAY_SIZE(sc89619d_settings_##inst),                         \
		.enable_charging = DT_INST_PROP(inst, enable_charging),                         \
		.configured = DT_INST_NODE_HAS_PROP(inst, constant_charge_current_max_microamp)  \
			| (DT_INST_NODE_HAS_PROP(inst, precharge_current_microamp) << 1)         \
			| (DT_INST_NODE_HAS_PROP(inst, charge_term_current_microamp) << 2)       \
			| (DT_INST_NODE_HAS_PROP(inst, input_current_limit_microamp) << 3)       \
			| (DT_INST_NODE_HAS_PROP(inst, southchip_charge_voltage_base_microvolt) << 4), \
	};                                                                                       \
	DEVICE_DT_INST_DEFINE(inst, sc89619d_init, NULL, &sc89619d_data_##inst,                    \
			      &sc89619d_config_##inst, POST_KERNEL, CONFIG_CHARGER_INIT_PRIORITY, \
			      &sc89619d_api);

DT_INST_FOREACH_STATUS_OKAY(SC89619D_DEFINE)
