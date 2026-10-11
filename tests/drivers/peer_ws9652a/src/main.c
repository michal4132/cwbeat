/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mfd/ws9652a.h>
#include <zephyr/drivers/misc/ws9652a_bus.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/soc/ws9652a/counter.h>
#include <zephyr/irq_offload.h>

/* The production peer driver addresses ordinary test storage as MMIO. */
static uint32_t bt_regs[1], mapping_regs[2], dsp_regs[12];
static uint32_t controller_regs[1], policy_regs[1];
static const struct device pmu = {.name = "test-pmu"};
static const struct device register_bus = {.name = "test-register-bus"};
static int mock_watchdog_disable(const struct device *dev);
static DEVICE_API(wdt, watchdog_api) = {.disable = mock_watchdog_disable};
static const struct device watchdog = {.name = "test-watchdog", .api = &watchdog_api};
static const pinctrl_soc_pin_t pins[] = {{0}};
static const struct pinctrl_state pin_states[] = {
	{.pins = pins, .pin_cnt = ARRAY_SIZE(pins), .id = PINCTRL_STATE_DEFAULT},
};
static const struct pinctrl_dev_config pin_config = {
	.states = pin_states,
	.state_cnt = ARRAY_SIZE(pin_states),
};
static bool pmu_ready, pmu_result, bus_ready, watchdog_ready;
static unsigned int watchdog_calls;
static int watchdog_result;
static unsigned int pinctrl_calls, pmu_calls;
static int pinctrl_result;
static unsigned int bus_calls;
static int bus_result;
static bool bus_admitted;
static k_tid_t bus_owner;
static unsigned int bus_depth;
static uint32_t close_bt, unlock_bt;
static bool unlock_admitted;
K_MUTEX_DEFINE(bus_mutex);
K_SEM_DEFINE(callback_entered, 0, 1);
K_SEM_DEFINE(callback_release, 0, 1);
K_SEM_DEFINE(release_entered, 0, 1);
K_SEM_DEFINE(release_finished, 0, 1);

static bool mock_ready(const struct device *dev)
{
	if (dev == &watchdog) {
		return watchdog_ready;
	}
	if (dev == &pmu) {
		return pmu_ready;
	}
	zassert_equal_ptr(dev, &register_bus);
	return bus_ready;
}

static int mock_watchdog_disable(const struct device *dev)
{
	zassert_equal_ptr(dev, &watchdog);
	zassert_equal(bt_regs[0] & 0x0c, 0, "BT must be held before stopping its watchdog");
	watchdog_calls++;
	return watchdog_result;
}

#if defined(CONFIG_PINCTRL)
/* Exercise the standard pinctrl state lookup against a traced provider.
 * The other fixture intentionally has no pinctrl provider symbol. */
int pinctrl_configure_pins(const pinctrl_soc_pin_t *selected, uint8_t count, uintptr_t reg)
{
	zassert_equal_ptr(selected, pins);
	zassert_equal(count, ARRAY_SIZE(pins));
	zassert_equal(reg, PINCTRL_REG_NONE);
	pinctrl_calls++;
	return pinctrl_result;
}
#endif

static bool mock_pmu_prepare(const struct device *dev)
{
	zassert_equal_ptr(dev, &pmu);
	pmu_calls++;
	zassert_true(bus_admitted, "PMU preparation requires the verified held window");
	zassert_not_equal(bus_owner, k_current_get(), "PMU must acquire its lock before domain");
	return pmu_result;
}

static uint32_t mock_bus_control(const struct device *dev)
{
	zassert_equal_ptr(dev, &pmu);
	return 0;
}

#define device_is_ready         mock_ready
#define ws9652a_pmu_prepare     mock_pmu_prepare
#define ws9652a_pmu_bus_control mock_bus_control
#define ws9652a_counter_read()  0u
#undef DT_INST_FOREACH_STATUS_OKAY
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#undef DT_REG_ADDR_BY_NAME
#define DT_REG_ADDR_BY_NAME(node, name) ((uintptr_t)name##_regs)
#undef DEVICE_DT_GET
#define DEVICE_DT_GET(node) (&watchdog)
#include "../../../../drivers/peer/ws9652a/peer_ws9652a.c"

static struct peer_data data;
static const struct peer_config config = {
	.bt = (uintptr_t)bt_regs,
	.mapping = (uintptr_t)mapping_regs,
	.dsp = (uintptr_t)dsp_regs,
	.watchdog = &watchdog,
	.controller = (uintptr_t)controller_regs,
	.policy = (uintptr_t)policy_regs,
	.pmu = &pmu,
	.register_bus = &register_bus,
	.pcfg = &pin_config,
	.pinctrl_present = true,
};
static const struct device peer = {.name = "test-peer", .config = &config, .data = &data};

int ws9652a_bus_lock(const struct device *dev, bool require_held)
{
	zassert_equal_ptr(dev, &register_bus);
	bus_calls++;
	if (bus_result) {
		return bus_result;
	}
	if (k_mutex_lock(&bus_mutex, K_MSEC(50))) {
		return -EBUSY;
	}
	if (require_held && !bus_admitted) {
		k_mutex_unlock(&bus_mutex);
		return -EBUSY;
	}
	bus_owner = k_current_get();
	bus_depth++;
	return 0;
}

void ws9652a_bus_unlock(const struct device *dev)
{
	zassert_equal_ptr(dev, &register_bus);
	zassert_equal(bus_owner, k_current_get());
	zassert_true(bus_depth > 0);
	unlock_bt = bt_regs[0];
	unlock_admitted = bus_admitted;
	if (--bus_depth == 0) {
		bus_owner = NULL;
	}
	k_mutex_unlock(&bus_mutex);
}

void ws9652a_bus_set_peers_held_locked(const struct device *dev, bool held)
{
	zassert_equal_ptr(dev, &register_bus);
	zassert_equal(bus_owner, k_current_get());
	if (held) {
		zassert_equal(bt_regs[0] & BT_RUN_MASK, 0);
		zassert_equal(dsp_regs[8 / 4] & (DSP_COMMAND_MASK | DSP_COMMAND_PENDING),
			      DSP_HOLD_COMMAND);
		zassert_true(dsp_regs[12 / 4] & DSP_HOLD_ACK);
	} else {
		close_bt = bt_regs[0];
	}
	bus_admitted = held;
}

static int early_pins;
static bool early_prepare, early_hold, early_dsp_hold;

static int before_kernel(void)
{
	early_pins = ws9652a_peer_prepare_pins(&peer);
	early_prepare = ws9652a_peer_prepare_power(&peer);
	early_hold = ws9652a_peer_hold(&peer);
	early_dsp_hold = ws9652a_peer_dsp_hold(&peer);
	ws9652a_peer_bt_release(&peer, UINT32_MAX);
	return 0;
}
SYS_INIT(before_kernel, PRE_KERNEL_2, 99);

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	memset(&data, 0, sizeof(data));
	memset(bt_regs, 0, sizeof(bt_regs));
	memset(dsp_regs, 0, sizeof(dsp_regs));
	dsp_regs[12 / 4] = DSP_HOLD_ACK;
	pmu_ready = true;
	bus_ready = true;
	watchdog_ready = true;
	watchdog_calls = 0;
	watchdog_result = 0;
	pmu_result = true;
	pinctrl_calls = 0;
	pmu_calls = 0;
	pinctrl_result = 0;
	bus_calls = 0;
	bus_result = 0;
	bus_admitted = false;
	bus_owner = NULL;
	bus_depth = 0;
	close_bt = unlock_bt = 0;
	unlock_admitted = false;
	k_mutex_init(&bus_mutex);
	k_sem_reset(&callback_entered);
	k_sem_reset(&callback_release);
	k_sem_reset(&release_entered);
	k_sem_reset(&release_finished);
	zassert_ok(init(&peer));
}

static void in_isr(const void *unused)
{
	ARG_UNUSED(unused);
	zassert_equal(ws9652a_peer_prepare_pins(&peer), -EWOULDBLOCK);
	zassert_false(ws9652a_peer_prepare_power(&peer));
	zassert_false(ws9652a_peer_hold(&peer));
	zassert_false(ws9652a_peer_dsp_hold(&peer));
	ws9652a_peer_bt_release(&peer, UINT32_MAX);
}

ZTEST(ws9652a_peer, test_lifecycle_context_refusal_has_no_hardware_effect)
{
	zassert_equal(early_pins, -EAGAIN);
	zassert_false(early_prepare);
	zassert_false(early_hold);
	zassert_false(early_dsp_hold);
	dsp_regs[8 / 4] = DSP_HOLD_COMMAND;
	irq_offload(in_isr, NULL);
	zassert_equal(bt_regs[0], 0);
	zassert_equal(dsp_regs[8 / 4], DSP_HOLD_COMMAND);
	zassert_equal(dsp_regs[12 / 4], DSP_HOLD_ACK);
	zassert_equal(bus_calls, 0);
	zassert_equal(pmu_calls, 0);
	zassert_equal(pinctrl_calls, 0);
	zassert_equal(watchdog_calls, 0);
}

ZTEST(ws9652a_peer, test_admission_requires_verified_hold_and_precedes_pmu)
{
	zassert_equal(ws9652a_bus_lock(&register_bus, true), -EBUSY);
	bt_regs[0] = BIT(7) | BT_RUN_MASK;
	zassert_true(ws9652a_peer_prepare_power(&peer));
	zassert_equal(watchdog_calls, 1);
	zassert_true(bus_admitted);
	zassert_equal(pmu_calls, 1);
	zassert_equal(ws9652a_peer_power_state(&peer), IPC_POLICY_READY);
	zassert_ok(ws9652a_bus_lock(&register_bus, true));
	ws9652a_bus_unlock(&register_bus);

	/* A policy failure does not revoke an independently verified peer hold. */
	pmu_result = false;
	zassert_false(ws9652a_peer_prepare_power(&peer));
	zassert_equal(ws9652a_peer_power_state(&peer), IPC_POLICY_FAILED);
	zassert_true(bus_admitted);

	/* Refreshing the hold with no DSP acknowledgement closes the old window. */
	dsp_regs[12 / 4] = 0;
	zassert_false(ws9652a_peer_prepare_power(&peer));
	zassert_equal(ws9652a_peer_power_state(&peer), IPC_POLICY_HOLD_FAILED);
	zassert_false(bus_admitted);
	zassert_equal(pmu_calls, 2);
	zassert_equal(ws9652a_bus_lock(&register_bus, true), -EBUSY);
}

ZTEST(ws9652a_peer, test_hold_paths_refresh_admission_and_emergency_never_opens_it)
{
	zassert_true(ws9652a_peer_hold_emergency());
	zassert_equal(watchdog_calls, 1);
	zassert_true(ws9652a_peer_dsp_hold_emergency());
	zassert_equal(watchdog_calls, 1);
	zassert_false(bus_admitted);
	zassert_equal(bus_calls, 0);

	bt_regs[0] = BT_RUN_MASK;
	zassert_true(ws9652a_peer_dsp_hold(&peer));
	zassert_false(bus_admitted);
	zassert_true(ws9652a_peer_hold(&peer));
	zassert_true(bus_admitted);
	dsp_regs[12 / 4] = 0;
	zassert_false(ws9652a_peer_dsp_hold(&peer));
	zassert_false(bus_admitted);
	dsp_regs[12 / 4] = DSP_HOLD_ACK;
	zassert_true(ws9652a_peer_dsp_hold(&peer));
	zassert_true(bus_admitted);
	dsp_regs[12 / 4] = 0;
	zassert_false(ws9652a_peer_hold(&peer));
	zassert_false(bus_admitted);
	zassert_equal(pmu_calls, 0);
	zassert_equal(watchdog_calls, 3);
}

ZTEST(ws9652a_peer, test_already_disabled_watchdog_allows_hold)
{
	watchdog_result = -EFAULT;
	bt_regs[0] = BT_RUN_MASK;
	zassert_true(ws9652a_peer_hold(&peer));
	zassert_equal(watchdog_calls, 1);
	zassert_true(bus_admitted);
}

ZTEST(ws9652a_peer, test_domain_failure_leaves_hardware_and_admission_unchanged)
{
	bt_regs[0] = BT_RUN_MASK | BIT(7);
	dsp_regs[8 / 4] = BIT(7);
	const int failures[] = {-EBUSY, -EIO};
	for (size_t i = 0; i < ARRAY_SIZE(failures); i++) {
		bus_result = failures[i];
		zassert_false(ws9652a_peer_prepare_power(&peer));
		zassert_false(ws9652a_peer_hold(&peer));
		zassert_false(ws9652a_peer_dsp_hold(&peer));
		zassert_equal(bt_regs[0], BT_RUN_MASK | BIT(7));
		zassert_equal(dsp_regs[8 / 4], BIT(7));
		zassert_false(bus_admitted);
	}
	bus_result = -EIO;
	ws9652a_peer_bt_release(&peer, UINT32_MAX);
	zassert_equal(bt_regs[0], BT_RUN_MASK | BIT(7));
	zassert_equal(pmu_calls, 0);
	zassert_equal(watchdog_calls, 0);
}

ZTEST(ws9652a_peer, test_release_closes_admission_before_run_bits)
{
	zassert_true(ws9652a_peer_hold(&peer));
	bt_regs[0] = BIT(7);
	ws9652a_peer_bt_release(&peer, BIT(7));
	zassert_equal(close_bt, BIT(7));
	zassert_equal(unlock_bt, BIT(7) | BT_RUN_MASK);
	zassert_false(unlock_admitted);
	zassert_false(bus_admitted);
	zassert_equal(ws9652a_bus_lock(&register_bus, true), -EBUSY);
}

K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, 2, 1024);
static struct k_thread workers[2];
static int worker_result;

static void bus_worker(void *unused0, void *unused1, void *unused2)
{
	worker_result = ws9652a_bus_lock(&register_bus, true);
	if (!worker_result) {
		k_sem_give(&callback_entered);
		zassert_ok(k_sem_take(&callback_release, K_SECONDS(2)));
		ws9652a_bus_unlock(&register_bus);
	}
}

static void release_worker(void *unused0, void *unused1, void *unused2)
{
	k_sem_give(&release_entered);
	ws9652a_peer_bt_release(&peer, BIT(7));
	k_sem_give(&release_finished);
}

static void start_worker(unsigned int slot, k_thread_entry_t entry)
{
	k_thread_create(&workers[slot], worker_stacks[slot],
			K_THREAD_STACK_SIZEOF(worker_stacks[slot]), entry, NULL, NULL, NULL,
			1, 0, K_NO_WAIT);
}

ZTEST(ws9652a_peer, test_transfer_excludes_release_and_lifecycle_changes)
{
	zassert_true(ws9652a_peer_hold(&peer));
	worker_result = -EINPROGRESS;
	start_worker(0, bus_worker);
	zassert_ok(k_sem_take(&callback_entered, K_SECONDS(1)));
	/* Hold refresh cannot mutate the command during an active transfer. */
	zassert_false(ws9652a_peer_hold(&peer));
	zassert_false(ws9652a_peer_dsp_hold(&peer));
	zassert_false(ws9652a_peer_prepare_power(&peer));
	zassert_equal(bt_regs[0], 0);
	zassert_equal(dsp_regs[8 / 4], DSP_HOLD_COMMAND);
	zassert_true(bus_admitted);
	start_worker(1, release_worker);
	zassert_ok(k_sem_take(&release_entered, K_SECONDS(1)));
	zassert_equal(k_sem_take(&release_finished, K_MSEC(1)), -EAGAIN);
	zassert_equal(bt_regs[0], 0);
	zassert_false(ws9652a_peer_hold(&peer));
	zassert_false(ws9652a_peer_dsp_hold(&peer));
	zassert_false(ws9652a_peer_prepare_power(&peer));
	zassert_equal(ws9652a_peer_prepare_pins(&peer), -EBUSY);
	zassert_equal(ws9652a_bus_lock(&register_bus, true), -EBUSY);
	zassert_equal(pmu_calls, 0);
	zassert_equal(bt_regs[0], 0);
	zassert_equal(dsp_regs[8 / 4], DSP_HOLD_COMMAND);
	k_sem_give(&callback_release);
	for (size_t i = 0; i < ARRAY_SIZE(workers); i++) {
		zassert_ok(k_thread_join(&workers[i], K_SECONDS(1)));
	}
	zassert_ok(worker_result);
	zassert_ok(k_sem_take(&release_finished, K_NO_WAIT));
	zassert_equal(bt_regs[0], BIT(7) | BT_RUN_MASK);
	zassert_false(bus_admitted);
	zassert_equal(ws9652a_bus_lock(&register_bus, true), -EBUSY);
}

ZTEST(ws9652a_peer, test_pins_require_successful_power_preparation)
{
	const uint32_t rejected[] = {
		IPC_POLICY_IDLE,
		IPC_POLICY_HOLDING,
		IPC_POLICY_HOLD_FAILED,
		IPC_POLICY_FAILED,
	};

	for (size_t i = 0; i < ARRAY_SIZE(rejected); i++) {
		data.preflight[IPC_POLICY_STATE] = rejected[i];
		zassert_equal(ws9652a_peer_prepare_pins(&peer), -EPERM);
	}
	zassert_equal(pinctrl_calls, 0);
}

ZTEST(ws9652a_peer, test_pins_require_bt_still_held)
{
	zassert_true(ws9652a_peer_prepare_power(&peer));
	zassert_equal(ws9652a_peer_power_state(&peer), IPC_POLICY_READY);
	for (uint32_t run = BIT(2); run <= BT_RUN_MASK; run += BIT(2)) {
		bt_regs[0] = run;
		zassert_equal(ws9652a_peer_prepare_pins(&peer), -EPERM);
	}
	zassert_equal(pinctrl_calls, 0);
}

ZTEST(ws9652a_peer, test_init_and_hold_paths_do_not_prepare_pins)
{
	bt_regs[0] = UINT32_MAX;
	zassert_ok(init(&peer));
	zassert_equal(bt_regs[0], UINT32_MAX);
	pmu_ready = false;
	zassert_equal(init(&peer), -ENODEV);
	pmu_ready = true;
	bus_ready = false;
	zassert_equal(init(&peer), -ENODEV);
	bus_ready = true;
	watchdog_ready = false;
	zassert_equal(init(&peer), -ENODEV);
	watchdog_ready = true;
	zassert_equal(bt_regs[0], UINT32_MAX);
	zassert_equal(watchdog_calls, 0, "device init must leave the inherited watchdog running");
	zassert_true(ws9652a_peer_hold(&peer));
	zassert_true(ws9652a_peer_hold_emergency());
	zassert_true(ws9652a_peer_dsp_hold(&peer));
	ws9652a_peer_controller_stop(&peer);
	zassert_equal(pinctrl_calls, 0);
	zassert_equal(pmu_calls, 0);
}

ZTEST(ws9652a_peer, test_no_pinctrl_state_is_noop)
{
	struct peer_config minimal = config;
	struct device minimal_peer = {.name = "minimal-peer", .config = &minimal, .data = &data};

	minimal.pcfg = NULL;
	minimal.pinctrl_present = false;
	zassert_true(ws9652a_peer_prepare_power(&minimal_peer));
	zassert_ok(ws9652a_peer_prepare_pins(&minimal_peer));
	zassert_equal(pinctrl_calls, 0);
}

#if defined(CONFIG_PINCTRL)
ZTEST(ws9652a_peer, test_default_pinctrl_applied_after_preflight)
{
	zassert_true(ws9652a_peer_prepare_power(&peer));
	zassert_equal(pmu_calls, 1);
	zassert_equal(pinctrl_calls, 0);
	zassert_ok(ws9652a_peer_prepare_pins(&peer));
	zassert_equal(pinctrl_calls, 1);
}

ZTEST(ws9652a_peer, test_missing_default_state_propagates_lookup_error)
{
	struct pinctrl_state sleep = pin_states[0];
	struct pinctrl_dev_config sleep_config = {.states = &sleep, .state_cnt = 1};
	struct peer_config no_default = config;
	struct device no_default_peer = {.name = "no-default-peer", .config = &no_default, .data = &data};

	sleep.id = PINCTRL_STATE_SLEEP;
	no_default.pcfg = &sleep_config;
	zassert_true(ws9652a_peer_prepare_power(&no_default_peer));
	zassert_equal(ws9652a_peer_prepare_pins(&no_default_peer), -ENOENT);
	zassert_equal(pinctrl_calls, 0);
}

ZTEST(ws9652a_peer, test_pinctrl_error_propagates_with_bt_held)
{
	zassert_true(ws9652a_peer_prepare_power(&peer));
	pinctrl_result = -EIO;
	zassert_equal(ws9652a_peer_prepare_pins(&peer), -EIO);
	zassert_equal(pinctrl_calls, 1);
	zassert_equal(ws9652a_peer_power_state(&peer), IPC_POLICY_READY);
	zassert_equal(bt_regs[0] & BT_RUN_MASK, 0);
}
#else
ZTEST(ws9652a_peer, test_declared_pinctrl_state_requires_provider)
{
	zassert_true(ws9652a_peer_prepare_power(&peer));
	zassert_equal(ws9652a_peer_prepare_pins(&peer), -ENOTSUP);
	zassert_equal(pinctrl_calls, 0);
}
#endif

ZTEST_SUITE(ws9652a_peer, NULL, NULL, reset, NULL, NULL);
