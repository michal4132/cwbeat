/* SPDX-License-Identifier: Apache-2.0 */
/* Controlled observations of the 22 kOhm pull-down control, formerly CONFIG0C. */
#include <errno.h>
#include <stdint.h>
#include <cwbeat/boards/jbl_go4/recovery.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/peer/ws9652a.h>
#include <zephyr/drivers/pinctrl/ws9652a.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define BUTTON_NODE DT_NODELABEL(volume_up_button)
#define BUTTON_PORT DT_GPIO_CTLR(BUTTON_NODE, gpios)
#define BUTTON_PIN DT_GPIO_PIN(BUTTON_NODE, gpios)
#define PAD_BASE DT_REG_ADDR_BY_NAME(BUTTON_PORT, port)
#define PAD_BIT BIT(BUTTON_PIN)
#define INPUT_OFFSET 0x10u
#define DEFAULT_DURATION_MS 2000u
#define MIN_DURATION_MS 100u
#define MAX_DURATION_MS 10000u
#define RECOVERY_DURATION_MS 100u
#define TRACE_EDGE_CAPACITY 128u
#define FIXTURE_VERSION "trace_v2"

BUILD_ASSERT(DT_SAME_NODE(BUTTON_PORT, DT_NODELABEL(gpio0)), "Only GPIO0 is supported");
BUILD_ASSERT(DT_PROP(BUTTON_PORT, bank) == 0 && BUTTON_PIN == 5,
	     "This experiment is restricted to the Go 4 Volume+ pad GPIO0.5");
BUILD_ASSERT(DT_GPIO_FLAGS(BUTTON_NODE, gpios) & GPIO_PULL_DOWN,
	     "The test button must start with its pull-down enabled");

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);

enum pad_register {
	OUTPUT, MODE, POLARITY, CONFIG0C, IRQ_MASK, ROUTE30, ROUTE38, ROUTE3C, PAD_REG_COUNT
};

static const uint8_t offsets[PAD_REG_COUNT] = {0x00, 0x04, 0x08, 0x0c,
					      0x14, 0x30, 0x38, 0x3c};
static const char *const names[PAD_REG_COUNT] = {
	"OUTPUT", "MODE", "POLARITY", "CONFIG0C", "IRQ_MASK", "ROUTE30", "ROUTE38", "ROUTE3C"
};

struct pad_snapshot {
	uint32_t value[PAD_REG_COUNT];
};

struct observations {
	uint32_t samples, high, low, rising, falling;
	uint32_t config_mismatches, guard_conflicts, peer_failures;
	uint32_t first_ms, last_ms, run_start_ms, initial_run_ms;
	uint32_t gap_min_ms, gap_max_ms, retained, dropped, elapsed_ms;
	bool first_high, last_high;
};

struct edge_record {
	uint32_t t_ms, previous_run_ms;
	bool high;
};

struct input_transition {
	uint32_t before_raw, after_raw, before_ms, after_ms;
	bool observed;
};

static struct pad_snapshot startup_before, startup_after;
static struct ws9652a_peer_snapshot peers_before, peers_after;
static bool hold_ack;
/* 0: main still owns startup; 1: ready; negative: startup errno. */
static atomic_t startup_state;
K_MUTEX_DEFINE(command_lock);
/* Protected by command_lock; retain the saved bit if restoration fails. */
static bool restore_pending;
static uint32_t saved_config_bit;
/* Static storage keeps bounded traces off the shell stack; command_lock owns both. */
static struct edge_record window_edges[TRACE_EDGE_CAPACITY];
static struct edge_record recovery_edges[TRACE_EDGE_CAPACITY];

static void pad_read_locked(struct pad_snapshot *out)
{
	for (unsigned i = 0; i < PAD_REG_COUNT; ++i) {
		out->value[i] = sys_read32(PAD_BASE + offsets[i]);
	}
}

static void pad_read(struct pad_snapshot *out)
{
	k_spinlock_key_t key = k_spin_lock(&ws9652a_pad_lock);

	pad_read_locked(out);
	k_spin_unlock(&ws9652a_pad_lock, key);
}

static bool peers_held(const struct ws9652a_peer_snapshot *state)
{
	return !(state->bt & 0x0cu) && (state->dsp_control & 0x3fu) == 0x01u &&
	       (state->dsp_status & 0x10u);
}

static bool peers_still_held(void)
{
	struct ws9652a_peer_snapshot state;

	ws9652a_peer_snapshot_emergency(&state);
	return peers_held(&state);
}

/* Count observations of conflicts, not the number of differing register bits. */
static bool check_guards(const struct pad_snapshot *actual,
			 const struct pad_snapshot *expected, struct observations *counts)
{
	bool conflict = false;
	bool mismatch = ((actual->value[CONFIG0C] ^ expected->value[CONFIG0C]) & PAD_BIT) != 0;

	for (unsigned i = 0; i < PAD_REG_COUNT; ++i) {
		uint32_t mask = i == CONFIG0C ? ~PAD_BIT : UINT32_MAX;

		conflict |= ((actual->value[i] ^ expected->value[i]) & mask) != 0;
	}
	counts->config_mismatches += mismatch;
	counts->guard_conflicts += conflict;
	return !mismatch && !conflict;
}

static bool input_profile(const struct pad_snapshot *state)
{
	return (state->value[OUTPUT] & PAD_BIT) && (state->value[MODE] & PAD_BIT) &&
	       !(state->value[POLARITY] & PAD_BIT) && (state->value[IRQ_MASK] & PAD_BIT) &&
	       !((state->value[ROUTE30] | state->value[ROUTE38] | state->value[ROUTE3C]) &
		 PAD_BIT);
}

static void print_pad(const struct shell *sh, const char *label,
		      const struct pad_snapshot *state)
{
	shell_print(sh, "%s GPIO0 full register words:", label);
	for (unsigned i = 0; i < PAD_REG_COUNT; ++i) {
		shell_print(sh, "  %-8s +0x%02x = 0x%08x", names[i], offsets[i], state->value[i]);
	}
}

static void print_peers(const struct shell *sh, const char *label,
			const struct ws9652a_peer_snapshot *state)
{
	shell_print(sh, "%s: BT=0x%08x DSP_CONTROL=0x%08x DSP_STATUS=0x%08x held=%u",
		    label, state->bt, state->dsp_control, state->dsp_status, peers_held(state));
}

static int ready(const struct shell *sh)
{
	int state = atomic_get(&startup_state);

	if (state != 1) {
		shell_error(sh, "Capture unavailable: startup %s (state=%d)",
			    state == 0 ? "in progress" : "failed", state);
		return state == 0 ? -EAGAIN : state;
	}
	return 0;
}

static int parse_duration(const struct shell *sh, const char *text, uint32_t *duration)
{
	uint32_t value = 0;

	if (text == NULL) {
		*duration = DEFAULT_DURATION_MS;
		return 0;
	}
	if (*text == '\0') {
		goto invalid;
	}
	for (; *text; ++text) {
		if (*text < '0' || *text > '9' || value > (MAX_DURATION_MS - (*text - '0')) / 10u) {
			goto invalid;
		}
		value = value * 10u + (*text - '0');
	}
	if (value < MIN_DURATION_MS || value > MAX_DURATION_MS) {
		goto invalid;
	}
	*duration = value;
	return 0;
invalid:
	shell_error(sh, "duration_ms must contain decimal digits only, in 100..10000");
	return -EINVAL;
}

static void read_input(uint32_t *raw, uint32_t *ms, int64_t start)
{
	*raw = sys_read32(PAD_BASE + INPUT_OFFSET);
	*ms = (uint32_t)(k_uptime_get() - start);
}

/* Always restore only our bit, even if another register or bit changed. */
static bool restore_bit(struct input_transition *input, int64_t start)
{
	if (!restore_pending) {
		return true;
	}
	k_spinlock_key_t key = k_spin_lock(&ws9652a_pad_lock);
	uint32_t current = sys_read32(PAD_BASE + offsets[CONFIG0C]);

	if (input) {
		read_input(&input->before_raw, &input->before_ms, start);
	}
	sys_write32((current & ~PAD_BIT) | saved_config_bit, PAD_BASE + offsets[CONFIG0C]);
	if (input) {
		read_input(&input->after_raw, &input->after_ms, start);
		input->observed = true;
	}
	restore_pending = (sys_read32(PAD_BASE + offsets[CONFIG0C]) & PAD_BIT) != saved_config_bit;
	k_spin_unlock(&ws9652a_pad_lock, key);
	return !restore_pending;
}

static bool set_bit(struct pad_snapshot *expected, unsigned bit, struct observations *counts,
		    struct input_transition *input, int64_t start)
{
	struct pad_snapshot actual;
	k_spinlock_key_t key = k_spin_lock(&ws9652a_pad_lock);

	pad_read_locked(&actual);
	bool valid = check_guards(&actual, expected, counts);

	if (valid) {
		expected->value[CONFIG0C] = (actual.value[CONFIG0C] & ~PAD_BIT) |
					    (bit ? PAD_BIT : 0u);
		read_input(&input->before_raw, &input->before_ms, start);
		sys_write32(expected->value[CONFIG0C], PAD_BASE + offsets[CONFIG0C]);
		read_input(&input->after_raw, &input->after_ms, start);
		input->observed = true;
		pad_read_locked(&actual);
		valid = check_guards(&actual, expected, counts);
	}
	k_spin_unlock(&ws9652a_pad_lock, key);
	return valid;
}

/* Times describe sampled runs, not exact physical transition times. */
static void record_sample(struct observations *counts, struct edge_record *edges,
			  uint32_t t_ms, bool high)
{
	if (!counts->samples) {
		counts->first_high = high;
		counts->first_ms = t_ms;
		counts->run_start_ms = t_ms;
	} else {
		uint32_t gap = t_ms - counts->last_ms;

		if (counts->samples == 1 || gap < counts->gap_min_ms) {
			counts->gap_min_ms = gap;
		}
		counts->gap_max_ms = MAX(counts->gap_max_ms, gap);
		if (high != counts->last_high) {
			uint32_t run_ms = t_ms - counts->run_start_ms;

			if (!(counts->rising + counts->falling)) {
				counts->initial_run_ms = run_ms;
			}
			counts->rising += high;
			counts->falling += !high;
			if (counts->retained < TRACE_EDGE_CAPACITY) {
				edges[counts->retained++] = (struct edge_record){t_ms, run_ms, high};
			} else {
				++counts->dropped;
			}
			counts->run_start_ms = t_ms;
		}
	}
	counts->high += high;
	counts->low += !high;
	counts->last_high = high;
	counts->last_ms = t_ms;
	++counts->samples;
}

static bool observe_window(const struct pad_snapshot *expected, struct observations *counts,
			   struct edge_record *edges, uint32_t duration, int64_t start,
			   struct pad_snapshot *after)
{
	bool valid = true;

	while (valid && k_uptime_get() < start + duration) {
		if (!peers_still_held()) {
			++counts->peer_failures;
			valid = false;
			break;
		}
		uint32_t raw, t_ms;
		k_spinlock_key_t key = k_spin_lock(&ws9652a_pad_lock);

		pad_read_locked(after);
		valid = check_guards(after, expected, counts);
		read_input(&raw, &t_ms, start);
		k_spin_unlock(&ws9652a_pad_lock, key);
		if (!valid) {
			break;
		}
		record_sample(counts, edges, t_ms, (raw & PAD_BIT) != 0);
		k_msleep(1);
	}
	counts->elapsed_ms = (uint32_t)(k_uptime_get() - start);
	pad_read(after);
	valid &= check_guards(after, expected, counts);
	if (!peers_still_held()) {
		++counts->peer_failures;
		valid = false;
	}
	return valid;
}

static void print_transition(const struct shell *sh, const char *label,
			     const struct input_transition *input)
{
	if (!input->observed) {
		shell_print(sh, "%s: no write performed; raw INPUT before/after unknown", label);
		return;
	}
	shell_print(sh, "%s raw INPUT: before=0x%08x bit5=%u t_ms=%u; after=0x%08x bit5=%u t_ms=%u",
		    label, input->before_raw, !!(input->before_raw & PAD_BIT), input->before_ms,
		    input->after_raw, !!(input->after_raw & PAD_BIT), input->after_ms);
}

static void print_observations(const struct shell *sh, const struct observations *counts,
			       const struct edge_record *edges, bool valid)
{
	shell_print(sh, "Observed samples=%u high=%u low=%u rising=%u falling=%u elapsed_ms=%u",
		    counts->samples, counts->high, counts->low, counts->rising, counts->falling,
		    counts->elapsed_ms);
	if (counts->samples) {
		uint32_t span = counts->last_ms - counts->first_ms;

		shell_print(sh, "Sampled first=%u at_ms=%u last=%u at_ms=%u span_ms=%u",
			    counts->first_high, counts->first_ms, counts->last_high, counts->last_ms, span);
		shell_print(sh, "Sampled runs: initial_ms=%u final_ms=%u (bounded by observed samples)",
			    counts->rising + counts->falling ? counts->initial_run_ms : span,
			    counts->last_ms - counts->run_start_ms);
	} else {
		shell_print(sh, "Sampled first=unknown last=unknown; sampled runs=unknown");
	}
	if (counts->samples > 1) {
		shell_print(sh, "Sample gaps: min_ms=%u max_ms=%u", counts->gap_min_ms, counts->gap_max_ms);
	} else {
		shell_print(sh, "Sample gaps: min_ms=unknown max_ms=unknown");
	}
	shell_print(sh, "Edges retained=%u dropped=%u; times are relative to this observation start",
		    counts->retained, counts->dropped);
	for (unsigned i = 0; i < counts->retained; ++i) {
		shell_print(sh, "  edge t_ms=%u level=%u previous_sampled_run_ms=%u",
			    edges[i].t_ms, edges[i].high, edges[i].previous_run_ms);
	}
	shell_print(sh, "config_mismatches=%u guard_conflicts=%u peer_failures=%u valid=%u",
		    counts->config_mismatches, counts->guard_conflicts, counts->peer_failures, valid);
}

static int capture(const struct shell *sh, const unsigned *bits, unsigned windows,
		   uint32_t duration)
{
	struct pad_snapshot baseline, expected, after_window, after_restore, after_recovery;
	struct observations total = {0}, counts = {0}, recovery = {0};
	struct input_transition change = {0}, restoration = {0};
	bool valid = false;
	int result;

	k_mutex_lock(&command_lock, K_FOREVER);
	result = ready(sh);
	if (result) {
		goto unlock;
	}
	if (restore_pending || !peers_still_held()) {
		shell_error(sh, "Capture refused: restoration pending=%u, peers held=%u",
			    restore_pending, peers_still_held());
		result = -EBUSY;
		goto unlock;
	}
	pad_read(&baseline);
	print_pad(sh, "Before capture", &baseline);
	expected = startup_after;
	expected.value[CONFIG0C] = (expected.value[CONFIG0C] & ~PAD_BIT) |
				    (baseline.value[CONFIG0C] & PAD_BIT);
	if (!input_profile(&baseline) || !check_guards(&baseline, &expected, &total)) {
		shell_error(sh, "Capture refused: established input profile changed");
		result = -EIO;
		goto unlock;
	}
	expected = baseline;
	saved_config_bit = baseline.value[CONFIG0C] & PAD_BIT;
	restore_pending = true;
	if (windows > 1) {
		shell_print(sh, "Keep the external button state fixed across windows; raw INPUT bit 5 only.");
	} else {
		shell_print(sh, "Sampling raw INPUT bit 5; press/release Volume+ if testing activity.");
	}
	for (unsigned window = 0; window < windows; ++window) {
		counts = (struct observations){0};
		change = (struct input_transition){0};
		shell_print(sh, "Window %u/%u CONFIG0C[5]=%u requested_ms=%u requested_sleep_ms=1",
			    window + 1, windows, bits[window], duration);
		int64_t start = k_uptime_get();

		valid = peers_still_held();
		counts.peer_failures += !valid;
		if (valid) {
			valid = set_bit(&expected, bits[window], &counts, &change, start);
		}
		/* A failed write still receives endpoint checks, but admits no samples. */
		valid &= observe_window(&expected, &counts, window_edges, valid ? duration : 0,
					start, &after_window);
		valid &= counts.samples != 0;
		if (!valid) {
			result = -EIO;
			break;
		}
		if (window + 1 == windows) {
			break;
		}
		print_transition(sh, "CONFIG write", &change);
		print_observations(sh, &counts, window_edges, valid);
		print_pad(sh, "After window", &after_window);
	}
	/* Restore and observe recovery before printing the final window's trace. */
	int64_t recovery_start = k_uptime_get();
	bool restored = restore_bit(&restoration, recovery_start);

	pad_read(&after_restore);
	bool guards_valid = check_guards(&after_restore, &baseline, &total);

	total.peer_failures += !peers_still_held();
	bool recovery_valid = observe_window(&baseline, &recovery, recovery_edges,
					     RECOVERY_DURATION_MS, recovery_start, &after_recovery);

	/* A restored bit must also survive the monitored interval before another capture. */
	restore_pending |= (after_recovery.value[CONFIG0C] & PAD_BIT) != saved_config_bit;
	restored &= !restore_pending;
	recovery_valid &= recovery.samples != 0;
	total.config_mismatches += recovery.config_mismatches;
	total.guard_conflicts += recovery.guard_conflicts;
	total.peer_failures += recovery.peer_failures;
	if (!restored || !guards_valid || total.peer_failures || !recovery_valid) {
		result = -EIO;
	}
	print_transition(sh, "CONFIG write", &change);
	print_observations(sh, &counts, window_edges, valid);
	print_pad(sh, "After window", &after_window);
	print_transition(sh, "Restoration (new time origin)", &restoration);
	print_pad(sh, "After restoration", &after_restore);
	shell_print(sh, "Post-restoration read-only observation requested_ms=%u requested_sleep_ms=1",
		    RECOVERY_DURATION_MS);
	print_observations(sh, &recovery, recovery_edges, recovery_valid);
	print_pad(sh, "After recovery observation", &after_recovery);
	shell_print(sh, "Restored CONFIG0C[5]=%u: %s; final config_mismatches=%u guard_conflicts=%u peer_failures=%u",
		    saved_config_bit != 0, restored ? "verified" : "FAILED",
		    total.config_mismatches, total.guard_conflicts, total.peer_failures);
	if (result) {
		shell_error(sh, "Capture invalid (%d); observations do not establish CONFIG0C's function",
			    result);
	} else {
		shell_print(sh, "Capture complete; observations do not establish CONFIG0C's function");
	}
unlock:
	k_mutex_unlock(&command_lock);
	return result;
}

static int cmd_sample(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t duration;

	if ((argv[1][0] != '0' && argv[1][0] != '1') || argv[1][1] != '\0') {
		shell_error(sh, "CONFIG0C value must be exactly 0 or 1");
		return -EINVAL;
	}
	int result = parse_duration(sh, argc == 3 ? argv[2] : NULL, &duration);
	unsigned bit = argv[1][0] - '0';

	return result ? result : capture(sh, &bit, 1, duration);
}

static int cmd_compare(const struct shell *sh, size_t argc, char **argv)
{
	const unsigned bits[] = {1, 0, 1};
	uint32_t duration;
	int result = parse_duration(sh, argc == 2 ? argv[1] : NULL, &duration);

	return result ? result : capture(sh, bits, ARRAY_SIZE(bits), duration);
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	k_mutex_lock(&command_lock, K_FOREVER);
	int state = atomic_get(&startup_state);

	shell_print(sh, "CONFIG0C fixture=" FIXTURE_VERSION
		    "; GPIO0.5 Volume+; startup_state=%d restoration_pending=%u", state, restore_pending);
	if (state != 0) {
		struct pad_snapshot current;
		struct ws9652a_peer_snapshot peers;

		shell_print(sh, "Startup hold acknowledged=%u", hold_ack);
		print_peers(sh, "Before hold", &peers_before);
		print_peers(sh, "After hold", &peers_after);
		ws9652a_peer_snapshot_emergency(&peers);
		print_peers(sh, "Current peers", &peers);
		print_pad(sh, "Before input setup", &startup_before);
		print_pad(sh, "After input setup", &startup_after);
		pad_read(&current);
		print_pad(sh, "Current", &current);
	}
	k_mutex_unlock(&command_lock);
	return 0;
}

static int cmd_recovery(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	k_mutex_lock(&command_lock, K_FOREVER);
	if (atomic_get(&startup_state) == 0) {
		shell_error(sh, "Startup in progress; retry recovery once startup completes");
		k_mutex_unlock(&command_lock);
		return -EAGAIN;
	}
	if (!restore_bit(NULL, 0)) {
		shell_error(sh, "CONFIG0C restoration failed; explicit recovery will still be requested");
	}
	shell_print(sh, "Requesting USB boot-loader recovery for reflashing");
	k_msleep(100);
	int result = recovery_request_immediate();

	shell_error(sh, "Recovery request returned %d", result);
	k_mutex_unlock(&command_lock);
	return result;
}

SHELL_STATIC_SUBCMD_SET_CREATE(config0c_commands,
	SHELL_CMD_ARG(status, NULL, "Show startup/current register and peer snapshots", cmd_status, 1, 0),
	SHELL_CMD_ARG(sample, NULL, "<0|1> [duration_ms]: sample raw Volume+ input", cmd_sample, 2, 1),
	SHELL_CMD_ARG(compare, NULL, "[duration_ms]: sample 1, 0, 1 with the same external state",
		      cmd_compare, 1, 1),
	SHELL_CMD_ARG(recovery, NULL, "Restore pending bit and request USB boot-loader recovery",
		      cmd_recovery, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(config0c, &config0c_commands, "Go 4 CONFIG0C hardware observations", NULL);

int main(void)
{
	pad_read(&startup_before);
	ws9652a_peer_snapshot_emergency(&peers_before);
	hold_ack = ws9652a_peer_hold_emergency();
	ws9652a_peer_snapshot_emergency(&peers_after);
	int result = -EIO;

	if (hold_ack && peers_held(&peers_after)) {
		result = gpio_is_ready_dt(&button) ? gpio_pin_configure_dt(&button, GPIO_INPUT) :
						  -ENODEV;
	}
	pad_read(&startup_after);
	if (!result && (!input_profile(&startup_after) ||
			!(startup_after.value[CONFIG0C] & PAD_BIT) || !peers_still_held())) {
		result = -EIO;
	}
	/* Atomic publication makes all preceding startup snapshots visible to the shell. */
	atomic_set(&startup_state, result ? result : 1);
	printk("CONFIG0C diagnostic " FIXTURE_VERSION " GPIO0.5: %s (%d). Run config0c status.\n",
	       result ? "startup failed; capture disabled" : "ready", result);
	return 0;
}
