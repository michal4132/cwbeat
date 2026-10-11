/* SPDX-License-Identifier: Apache-2.0 */
#include <cwbeat/battery.h>
#include <zephyr/drivers/mfd/ws9652a.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/shell/shell.h>
#include <errno.h>
#include <stdlib.h>

#define PMU_NODE                  DT_COMPAT_GET_ANY_STATUS_OKAY(zgmicro_ws9652a_pmu)
#define ADC_NODE                  DT_COMPAT_GET_ANY_STATUS_OKAY(zgmicro_ws9652a_adc)
#define PROBE_MAX_SAMPLES         10000
#define PROBE_MIN_PERIOD_MS       10
#define PROBE_MAX_PERIOD_MS       60000
#define DECIMAL_BASE              10
#define BATTERY_MIN_PERIOD_MS     100
#define BATTERY_DEFAULT_PERIOD_MS 1000
#define PROBE_STACK_SIZE          1536
#define PROBE_PRIORITY            5

struct probe_state {
	bool active, battery;
	uint32_t target, attempts, samples, period_ms;
	uint32_t started_ms, elapsed_ms, max_read_us;
	int error;
	struct ws9652a_pmu_identity first, last;
	struct cwbeat_battery_sample last_battery;
	struct cwbeat_battery_diagnostic battery_trace;
	uint8_t min_adc, max_adc;
};

static struct probe_state state;
static struct k_spinlock state_lock;
/* Serialize start/cancel even if more than one shell backend is enabled. */
K_MUTEX_DEFINE(command_lock);
static void probe_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(probe_work, probe_handler);
static struct k_work_q probe_queue;
K_THREAD_STACK_DEFINE(probe_stack, PROBE_STACK_SIZE);

static int probe_init(void)
{
	const struct k_work_queue_config config = {.name = "pmu_probe"};
	k_work_queue_start(&probe_queue, probe_stack, K_THREAD_STACK_SIZEOF(probe_stack),
			   K_PRIO_PREEMPT(PROBE_PRIORITY), &config);
	return 0;
}
SYS_INIT(probe_init, APPLICATION, 80);

static bool same_identity(const struct ws9652a_pmu_identity *a,
			  const struct ws9652a_pmu_identity *b)
{
	return a->backend_c7 == b->backend_c7 && a->id_f5 == b->id_f5 && a->id_f4 == b->id_f4;
}

static void probe_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	bool active = state.active;
	bool battery = state.battery;
	k_spin_unlock(&state_lock, key);
	if (!active) {
		return;
	}

	struct ws9652a_pmu_identity sample;
	struct cwbeat_battery_sample battery_sample;
	struct cwbeat_battery_diagnostic trace = {
		.conversion.cleanup_status = WS9652A_ADC_UNOBSERVED,
	};
	int64_t started = k_uptime_ticks();
	int error;

	if (battery) {
#if defined(CONFIG_CWBEAT_BATTERY)
		error = cwbeat_battery_read(DEVICE_DT_GET(ADC_NODE), &battery_sample, &trace);
#else
		error = -ENOTSUP;
#endif
	} else {
		error = ws9652a_pmu_read_identity(DEVICE_DT_GET(PMU_NODE), &sample);
	}
	uint32_t duration_us = k_ticks_to_us_floor64(k_uptime_ticks() - started);

	key = k_spin_lock(&state_lock);
	++state.attempts;
	state.elapsed_ms = k_uptime_get_32() - state.started_ms;
	state.max_read_us = MAX(state.max_read_us, duration_us);
	if (battery) {
		state.battery_trace = trace;
	}
	if (!error) {
		if (battery) {
			state.last_battery = battery_sample;
			state.min_adc = state.samples ? MIN(state.min_adc, battery_sample.adc)
						      : battery_sample.adc;
			state.max_adc = MAX(state.max_adc, battery_sample.adc);
		} else {
			state.last = sample;
			if (!state.samples) {
				state.first = sample;
			} else if (!same_identity(&state.first, &sample)) {
				error = -EILSEQ;
			}
		}
		++state.samples;
	}
	state.error = error;
	state.active = state.active && !error && state.attempts < state.target;
	if (state.active) {
		/* Schedule before dropping the state lock so stop cannot cancel and
		 * then be overtaken by a late reschedule from this handler. */
		k_work_reschedule_for_queue(&probe_queue, &probe_work, K_MSEC(state.period_ms));
	}
	k_spin_unlock(&state_lock, key);
}

static int parse_number(const char *text, unsigned long min, unsigned long max, uint32_t *value)
{
	char *end;

	if (*text < '0' || *text > '9') {
		return -EINVAL;
	}
	errno = 0;
	unsigned long parsed = strtoul(text, &end, DECIMAL_BASE);
	if (errno || *end || parsed < min || parsed > max) {
		return -EINVAL;
	}
	*value = parsed;
	return 0;
}

static int start_probe(const struct shell *shell, size_t argc, char **argv, bool battery)
{
	uint32_t count = 1;
	uint32_t period_ms = battery ? BATTERY_DEFAULT_PERIOD_MS : PROBE_MIN_PERIOD_MS;
	uint32_t min_period_ms = battery ? BATTERY_MIN_PERIOD_MS : PROBE_MIN_PERIOD_MS;
	int error = argc > 1 ? parse_number(argv[1], 1, PROBE_MAX_SAMPLES, &count) : 0;

	if (!error && argc > 2) {
		error = parse_number(argv[2], min_period_ms, PROBE_MAX_PERIOD_MS, &period_ms);
	}
	if (error) {
		shell_error(shell, "count=1..%u period_ms=%u..%u", PROBE_MAX_SAMPLES, min_period_ms,
			    PROBE_MAX_PERIOD_MS);
		return error;
	}

	k_mutex_lock(&command_lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	bool active = state.active;
	k_spin_unlock(&state_lock, key);
	if (active) {
		k_mutex_unlock(&command_lock);
		return -EBUSY;
	}
	struct k_work_sync sync;
	k_work_cancel_delayable_sync(&probe_work, &sync);
	key = k_spin_lock(&state_lock);
	state = (struct probe_state){.active = true,
				     .battery = battery,
				     .target = count,
				     .period_ms = period_ms,
				     .started_ms = k_uptime_get_32()};
	k_spin_unlock(&state_lock, key);
	k_work_schedule_for_queue(&probe_queue, &probe_work, K_NO_WAIT);
	k_mutex_unlock(&command_lock);
	shell_print(shell, "PMU %s probe started: count=%u period_ms=%u",
		    battery ? "battery" : "identity", count, period_ms);
	return 0;
}

static int cmd_probe(const struct shell *shell, size_t argc, char **argv)
{
	return start_probe(shell, argc, argv, false);
}

#if defined(CONFIG_CWBEAT_BATTERY)
static int cmd_battery(const struct shell *shell, size_t argc, char **argv)
{
	return start_probe(shell, argc, argv, true);
}
#endif

static int cmd_status(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	struct probe_state snapshot = state;
	k_spin_unlock(&state_lock, key);
	shell_print(shell,
		    "PMU active=%u attempts=%u samples=%u target=%u period_ms=%u "
		    "elapsed_ms=%u max_read_us=%u error=%d mode=%s",
		    snapshot.active, snapshot.attempts, snapshot.samples, snapshot.target,
		    snapshot.period_ms, snapshot.elapsed_ms, snapshot.max_read_us, snapshot.error,
		    snapshot.battery ? "battery" : "identity");
	if (snapshot.battery) {
		const struct ws9652a_adc_diagnostic *trace = &snapshot.battery_trace.conversion;
		shell_print(shell,
			    "battery completed=%u prerequisite=%u reg=%02x value=%02x write=%u "
			    "bus=%x cleanup=%x blocked=%u",
			    snapshot.battery_trace.completed, trace->prerequisite, trace->index,
			    trace->value, trace->write, trace->bus_status, trace->cleanup_status,
			    trace->blocked);
		if (snapshot.samples) {
			const uint8_t *raw = snapshot.last_battery.raw;
			shell_print(shell, "battery adc=%u min=%u max=%u raw=%u,%u,%u,%u,%u,%u,%u",
				    snapshot.last_battery.adc, snapshot.min_adc, snapshot.max_adc,
				    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6]);
		}
	} else if (snapshot.samples) {
		shell_print(shell, "first C7=%02x F5=%02x F4=%02x; last C7=%02x F5=%02x F4=%02x",
			    snapshot.first.backend_c7, snapshot.first.id_f5, snapshot.first.id_f4,
			    snapshot.last.backend_c7, snapshot.last.id_f5, snapshot.last.id_f4);
	}
	return 0;
}

static int cmd_stop(const struct shell *shell, size_t argc, char **argv)
{
	k_mutex_lock(&command_lock, K_FOREVER);
	k_spinlock_key_t key = k_spin_lock(&state_lock);
	state.active = false;
	k_spin_unlock(&state_lock, key);
	struct k_work_sync sync;
	k_work_cancel_delayable_sync(&probe_work, &sync);
	k_mutex_unlock(&command_lock);
	return cmd_status(shell, argc, argv);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	pmu_subcommands,
#if defined(CONFIG_CWBEAT_BATTERY)
	SHELL_CMD_ARG(battery, NULL, "Sample battery ADC: [count] [period_ms]", cmd_battery, 1, 2),
#endif
	SHELL_CMD_ARG(probe, NULL, "Read C7/F5/F4: <count> [period_ms]", cmd_probe, 2, 1),
	SHELL_CMD_ARG(status, NULL, "Show probe progress and first failure", cmd_status, 1, 0),
	SHELL_CMD_ARG(stop, NULL, "Stop the probe and show its result", cmd_stop, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(pmu, &pmu_subcommands, "Experimental PMU validation", NULL);
