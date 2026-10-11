#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the actual CONFIG0C diagnostic handlers with a native C compiler.

Usage: python3 tests/boards/jbl_go4_config0c/verify_trace_native.py
Uses CC (default: cc). No Zephyr SDK, Python packages, hardware or flash access is
required. MMIO, peer state, scheduling and button levels are simulated; passing
does not identify CONFIG0C's electrical function or validate hardware timing.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


HEADERS = (
    "cwbeat/boards/jbl_go4/recovery.h",
    "zephyr/drivers/gpio.h",
    "zephyr/drivers/peer/ws9652a.h",
    "zephyr/drivers/pinctrl/ws9652a.h",
    "zephyr/kernel.h",
    "zephyr/shell/shell.h",
    "zephyr/sys/atomic.h",
    "zephyr/sys/printk.h",
    "zephyr/sys/sys_io.h",
    "zephyr/sys/util.h",
)

STUB = r"""
#ifndef CONFIG0C_HOST_STUB_H
#define CONFIG0C_HOST_STUB_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define BIT(n) (1u << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARG_UNUSED(a) (void)(a)
#define BUILD_ASSERT(c, m) _Static_assert(c, m)
#define DT_NODELABEL(n) 0
#define DT_GPIO_CTLR(n, g) 0
#define DT_GPIO_PIN(n, g) 5
#define DT_GPIO_FLAGS(n, g) GPIO_PULL_DOWN
#define DT_REG_ADDR_BY_NAME(n, g) 0x1000u
#define DT_SAME_NODE(a, b) 1
#define DT_PROP(n, p) 0
#define GPIO_DT_SPEC_GET(n, g) {0}
#define GPIO_INPUT 1
#define GPIO_PULL_DOWN 2
#define K_FOREVER -1
#define K_MUTEX_DEFINE(n) int n
#define SHELL_STATIC_SUBCMD_SET_CREATE(...)
#define SHELL_CMD_REGISTER(...)
struct gpio_dt_spec { int dummy; };
struct shell { int dummy; };
struct k_spinlock { int dummy; };
typedef int k_spinlock_key_t;
typedef int atomic_t;
struct ws9652a_peer_snapshot { uint32_t bt, dsp_control, dsp_status; };
extern struct k_spinlock ws9652a_pad_lock;
extern int host_spin_depth;
void ws9652a_peer_snapshot_emergency(struct ws9652a_peer_snapshot *out);
bool ws9652a_peer_hold_emergency(void);
bool gpio_is_ready_dt(const struct gpio_dt_spec *s);
int gpio_pin_configure_dt(const struct gpio_dt_spec *s, int flags);
uint32_t sys_read32(uintptr_t address);
void sys_write32(uint32_t value, uintptr_t address);
int64_t k_uptime_get(void);
void k_msleep(int ms);
void shell_print(const struct shell *s, const char *fmt, ...);
void shell_error(const struct shell *s, const char *fmt, ...);
void printk(const char *fmt, ...);
int recovery_request_immediate(void);
static inline int atomic_get(atomic_t *a) { return *a; }
static inline void atomic_set(atomic_t *a, int v) { *a = v; }
static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
    (void)lock;
    return host_spin_depth++;
}
static inline void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
    (void)lock;
    host_spin_depth = key;
}
static inline void k_mutex_lock(int *mutex, int timeout) { (void)mutex; (void)timeout; }
static inline void k_mutex_unlock(int *mutex) { (void)mutex; }
#endif
"""

HARNESS = r"""
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "stub.h"
#define main diagnostic_main
#include "main.c"
#undef main

struct k_spinlock ws9652a_pad_lock;
int host_spin_depth;
static uint32_t regs[16], written[64];
static unsigned writes, gpio_calls, cases, input_reads, sleep_calls;
static bool held, acknowledge, gpio_cfg_bit;
static int64_t now;
static unsigned sleep_extra_ms;
static unsigned cadence[8], cadence_length;
static char log_text[131072];
static size_t log_length;
static enum {
    NONE, GUARD_OTHER_CONFIG, GUARD_MODE, SELECTED_BIT, PEER_ESCAPE,
    DROP_CLEAR, DROP_RESTORE
} fault;
static bool injected;
static int64_t fault_at;
struct input_change { unsigned ms; bool high; };
static struct input_change stimulus[16];
static unsigned stimulus_count;
static bool alternating_input, input_follows_config, check_restore_print_order;

uint32_t sys_read32(uintptr_t address)
{
    assert(address >= PAD_BASE && address < PAD_BASE + sizeof(regs));
    if (address == PAD_BASE + INPUT_OFFSET) {
        bool high = false;
        ++input_reads;
        if (input_follows_config) {
            high = (regs[3] & PAD_BIT) != 0;
        } else if (alternating_input) {
            high = input_reads % 2 != 0;
        } else {
            for (unsigned i = 0; i < stimulus_count && stimulus[i].ms <= now; ++i) {
                high = stimulus[i].high;
            }
        }
        return high ? PAD_BIT : 0;
    }
    return regs[(address - PAD_BASE) / 4];
}

void sys_write32(uint32_t value, uintptr_t address)
{
    assert(host_spin_depth > 0);
    assert(address == PAD_BASE + offsets[CONFIG0C]);
    assert(writes < ARRAY_SIZE(written));
    written[writes++] = value;
    if ((fault == DROP_CLEAR && !(value & PAD_BIT)) ||
        (fault == DROP_RESTORE && (value & PAD_BIT))) {
        return;
    }
    regs[(address - PAD_BASE) / 4] = value;
}

void ws9652a_peer_snapshot_emergency(struct ws9652a_peer_snapshot *out)
{
    *out = (struct ws9652a_peer_snapshot){held ? 0 : 0xc, 1, 0x10};
}
bool ws9652a_peer_hold_emergency(void) { return acknowledge; }
bool gpio_is_ready_dt(const struct gpio_dt_spec *spec) { (void)spec; return true; }
int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, int flags)
{
    (void)spec;
    assert(flags == GPIO_INPUT);
    ++gpio_calls;
    regs[0] |= PAD_BIT;
    regs[1] |= PAD_BIT;
    regs[2] &= ~PAD_BIT;
    if (gpio_cfg_bit) { regs[3] |= PAD_BIT; } else { regs[3] &= ~PAD_BIT; }
    regs[5] |= PAD_BIT;
    regs[12] &= ~PAD_BIT;
    regs[14] &= ~PAD_BIT;
    regs[15] &= ~PAD_BIT;
    return 0;
}

int64_t k_uptime_get(void) { return now; }
void k_msleep(int ms)
{
    assert(host_spin_depth == 0 && ms > 0);
    now += cadence_length ? cadence[sleep_calls % cadence_length] : ms + sleep_extra_ms;
    ++sleep_calls;
    assert(now < 100000);
    if (!injected && now >= fault_at) {
        injected = true;
        if (fault == GUARD_OTHER_CONFIG) { regs[3] ^= BIT(9); }
        if (fault == GUARD_MODE) { regs[1] ^= BIT(9); }
        if (fault == SELECTED_BIT) { regs[3] ^= PAD_BIT; }
        if (fault == PEER_ESCAPE) { held = false; }
    }
}

static void append_log(const char *fmt, va_list args)
{
    assert(host_spin_depth == 0);
    /* A final trace must not delay restoration or the immediate recovery sample. */
    if (check_restore_print_order && now >= 100 && now < 200) { assert(false); }
    int count = vsnprintf(log_text + log_length, sizeof(log_text) - log_length, fmt, args);
    assert(count >= 0 && (size_t)count + 1 < sizeof(log_text) - log_length);
    log_length += count;
    log_text[log_length++] = '\n';
    log_text[log_length] = '\0';
}
void shell_print(const struct shell *sh, const char *fmt, ...)
{
    (void)sh;
    va_list args;
    va_start(args, fmt);
    append_log(fmt, args);
    va_end(args);
}
void shell_error(const struct shell *sh, const char *fmt, ...)
{
    (void)sh;
    va_list args;
    va_start(args, fmt);
    append_log(fmt, args);
    va_end(args);
}
void printk(const char *fmt, ...) { (void)fmt; }
int recovery_request_immediate(void) { return -ENOSYS; }

static unsigned occurrences(const char *needle)
{
    unsigned count = 0;
    for (const char *p = log_text; (p = strstr(p, needle)); p += strlen(needle)) {
        ++count;
    }
    return count;
}

static void fresh(bool boot)
{
    for (unsigned i = 0; i < ARRAY_SIZE(regs); ++i) { regs[i] = 0xa555a580u; }
    memset(written, 0, sizeof(written));
    writes = gpio_calls = input_reads = sleep_calls = 0;
    held = acknowledge = gpio_cfg_bit = true;
    now = 0;
    fault = NONE;
    fault_at = 3;
    injected = alternating_input = false;
    input_follows_config = check_restore_print_order = false;
    stimulus_count = cadence_length = sleep_extra_ms = 0;
    hold_ack = restore_pending = false;
    startup_state = 0;
    saved_config_bit = 0;
    host_spin_depth = 0;
    log_length = 0;
    log_text[0] = '\0';
    if (boot) {
        assert(diagnostic_main() == 0);
        assert(startup_state == 1);
    }
}

static int sample(const char *value, const char *duration)
{
    char *args[] = {"sample", (char *)value, (char *)duration};
    return cmd_sample(NULL, duration ? 3 : 2, args);
}

static void test_parsing(void)
{
    fresh(true);
    uint32_t duration;
    assert(parse_duration(NULL, NULL, &duration) == 0 && duration == 2000);
    const char *bad[] = {"", "99", "0", "10001", "99999999999999999999", "-100",
                         "+100", " 100", "100 ", "1e3", "0x100", "100x"};
    for (unsigned i = 0; i < ARRAY_SIZE(bad); ++i) {
        assert(parse_duration(NULL, bad[i], &duration) == -EINVAL);
    }
    assert(parse_duration(NULL, "100", &duration) == 0 && duration == 100);
    assert(parse_duration(NULL, "10000", &duration) == 0 && duration == 10000);
    assert(parse_duration(NULL, "00100", &duration) == 0 && duration == 100);
    const char *values[] = {"", "2", "01", "10", " 1", "1 ", "-1", "true"};
    for (unsigned i = 0; i < ARRAY_SIZE(values); ++i) {
        assert(sample(values[i], "100") == -EINVAL);
    }
    assert(writes == 0);
    ++cases;
}

static void test_success_and_reset(void)
{
    fresh(true);
    uint32_t before[16];
    memcpy(before, regs, sizeof(regs));
    char *args[] = {"compare", "100"};
    assert(cmd_compare(NULL, 2, args) == 0);
    assert(writes == 4 && occurrences("Window ") == 3);
    assert((written[0] & PAD_BIT) && !(written[1] & PAD_BIT) &&
           (written[2] & PAD_BIT) && (written[3] & PAD_BIT));
    for (unsigned i = 0; i < writes; ++i) {
        assert((written[i] & ~PAD_BIT) == (before[3] & ~PAD_BIT));
    }
    assert(!memcmp(before, regs, sizeof(regs)) && !restore_pending);
    ++cases;

    fresh(true);
    regs[3] &= ~PAD_BIT;
    memcpy(before, regs, sizeof(regs));
    assert(sample("1", "100") == 0 && !(regs[3] & PAD_BIT) && !restore_pending);
    assert(!memcmp(before, regs, sizeof(regs)));
    ++cases;
}

static void test_failures(void)
{
    const int modes[] = {GUARD_OTHER_CONFIG, GUARD_MODE, SELECTED_BIT, PEER_ESCAPE, DROP_CLEAR};
    for (unsigned i = 0; i < ARRAY_SIZE(modes); ++i) {
        fresh(true);
        uint32_t before[16];
        memcpy(before, regs, sizeof(regs));
        fault = modes[i];
        assert(sample("0", "100") == -EIO);
        assert((regs[3] & PAD_BIT) && !restore_pending && writes == 2);
        if (fault == GUARD_OTHER_CONFIG) { before[3] ^= BIT(9); }
        if (fault == GUARD_MODE) { before[1] ^= BIT(9); }
        assert(!memcmp(before, regs, sizeof(regs)));
        ++cases;
    }
    fresh(true);
    fault = DROP_RESTORE;
    assert(sample("0", "100") == -EIO && restore_pending && !(regs[3] & PAD_BIT));
    unsigned before = writes;
    assert(sample("0", "100") == -EBUSY && writes == before);
    fault = NONE;
    assert(restore_bit(NULL, now) && !restore_pending && (regs[3] & PAD_BIT));
    assert(sample("0", "100") == 0);
    ++cases;

    fresh(true);
    held = false;
    assert(sample("0", "100") == -EBUSY && writes == 0);
    ++cases;
    fresh(true);
    regs[1] ^= BIT(9);
    assert(sample("0", "100") == -EIO && writes == 0);
    ++cases;
}

static void test_startup(void)
{
    fresh(false);
    acknowledge = false;
    uint32_t before[16];
    memcpy(before, regs, sizeof(regs));
    assert(diagnostic_main() == 0 && startup_state == -EIO);
    assert(gpio_calls == 0 && writes == 0 && !memcmp(before, regs, sizeof(regs)));
    assert(sample("1", "100") == -EIO && writes == 0);
    ++cases;
    fresh(false);
    held = false;
    memcpy(before, regs, sizeof(regs));
    assert(diagnostic_main() == 0 && startup_state == -EIO);
    assert(gpio_calls == 0 && writes == 0 && !memcmp(before, regs, sizeof(regs)));
    ++cases;
    fresh(false);
    gpio_cfg_bit = false;
    assert(diagnostic_main() == 0 && startup_state == -EIO && gpio_calls == 1);
    assert(sample("0", "100") == -EIO && writes == 0);
    ++cases;
    fresh(false);
    assert(sample("0", "100") == -EAGAIN && writes == 0);
    ++cases;
}

static void test_trace_timing(void)
{
    fresh(true);
    struct observations counts = {0};
    struct edge_record edges[TRACE_EDGE_CAPACITY];
    struct pad_snapshot after;
    cadence[0] = 2;
    cadence[1] = 3;
    cadence[2] = 1;
    cadence_length = 3;
    stimulus[0] = (struct input_change){3, true};
    stimulus[1] = (struct input_change){7, false};
    stimulus_count = 2;
    /* Samples at 0, 2, 5, 6, 8 ms; deadline is crossed on the next sleep. */
    assert(observe_window(&startup_after, &counts, edges, 10, 0, &after));
    assert(counts.samples == 5 && counts.high == 2 && counts.low == 3);
    assert(counts.rising == 1 && counts.falling == 1);
    assert(!counts.first_high && !counts.last_high);
    assert(counts.first_ms == 0 && counts.last_ms == 8 && counts.elapsed_ms == 11);
    assert(counts.gap_min_ms == 1 && counts.gap_max_ms == 3);
    assert(counts.initial_run_ms == 5 && counts.run_start_ms == 8);
    assert(counts.retained == 2 && counts.dropped == 0);
    assert(edges[0].t_ms == 5 && edges[0].high && edges[0].previous_run_ms == 5);
    assert(edges[1].t_ms == 8 && !edges[1].high && edges[1].previous_run_ms == 3);
    assert(log_length == 0); /* Acquisition must not print through the shell. */
    ++cases;

    fresh(true);
    counts = (struct observations){0};
    sleep_extra_ms = 1;
    stimulus[0] = (struct input_change){0, true};
    stimulus_count = 1;
    assert(observe_window(&startup_after, &counts, edges, 100, 0, &after));
    assert(counts.samples == 50 && counts.high == 50 && counts.low == 0);
    assert(counts.first_high && counts.last_high && counts.retained == 0);
    assert(counts.first_ms == 0 && counts.last_ms == 98 && counts.elapsed_ms == 100);
    assert(counts.gap_min_ms == 2 && counts.gap_max_ms == 2);
    print_observations(NULL, &counts, edges, true);
    assert(strstr(log_text, "Sampled runs: initial_ms=98 final_ms=98"));
    ++cases;

    fresh(true);
    counts = (struct observations){0};
    now = 3;
    stimulus[0] = (struct input_change){5, true};
    stimulus_count = 1;
    assert(observe_window(&startup_after, &counts, edges, 7, 0, &after));
    assert(counts.samples == 4 && counts.first_ms == 3 && counts.last_ms == 6);
    assert(counts.initial_run_ms == 2 && counts.run_start_ms == 5);
    assert(edges[0].t_ms == 5 && edges[0].previous_run_ms == 2);
    ++cases;
}

static void test_trace_overflow(void)
{
    fresh(true);
    struct observations counts = {0};
    struct {
        uint32_t before;
        struct edge_record edges[TRACE_EDGE_CAPACITY];
        uint32_t after;
    } guarded = {.before = 0x12345678, .after = 0xabcdef01};
    struct pad_snapshot after;
    alternating_input = true;
    assert(observe_window(&startup_after, &counts, guarded.edges, 300, 0, &after));
    assert(counts.samples == 300 && counts.high == 150 && counts.low == 150);
    assert(counts.rising == 149 && counts.falling == 150);
    assert(counts.retained == TRACE_EDGE_CAPACITY);
    assert(counts.dropped == 299 - TRACE_EDGE_CAPACITY);
    assert(counts.first_high && !counts.last_high && counts.last_ms == 299);
    assert(counts.run_start_ms == 299 && counts.elapsed_ms == 300);
    assert(guarded.before == 0x12345678 && guarded.after == 0xabcdef01);
    for (unsigned i = 0; i < counts.retained; ++i) {
        assert(guarded.edges[i].t_ms == i + 1);
        assert(guarded.edges[i].previous_run_ms == 1);
        assert(guarded.edges[i].high == (i % 2 != 0));
    }
    ++cases;
}

static void test_observation_endpoints(void)
{
    fresh(true);
    struct observations counts = {0};
    struct edge_record edges[TRACE_EDGE_CAPACITY];
    struct pad_snapshot after;
    now = 100;
    assert(observe_window(&startup_after, &counts, edges, 100, 0, &after));
    assert(counts.samples == 0 && counts.retained == 0);
    print_observations(NULL, &counts, edges, true);
    assert(strstr(log_text, "Sampled first=unknown last=unknown"));
    assert(strstr(log_text, "Sample gaps: min_ms=unknown max_ms=unknown"));
    ++cases;

    fresh(true);
    counts = (struct observations){0};
    fault = GUARD_MODE;
    fault_at = 100;
    assert(!observe_window(&startup_after, &counts, edges, 100, 0, &after));
    assert(counts.samples == 100 && counts.guard_conflicts == 1);
    ++cases;

    fresh(true);
    counts = (struct observations){0};
    fault = PEER_ESCAPE;
    fault_at = 100;
    assert(!observe_window(&startup_after, &counts, edges, 100, 0, &after));
    assert(counts.samples == 100 && counts.peer_failures == 1);
    ++cases;
}

static void test_restoration_observation(void)
{
    const int modes[] = {GUARD_OTHER_CONFIG, GUARD_MODE, SELECTED_BIT, PEER_ESCAPE};
    for (unsigned i = 0; i < ARRAY_SIZE(modes); ++i) {
        fresh(true);
        fault = modes[i];
        fault_at = 103; /* Main window ends at 100; fail during restored observation. */
        assert(sample("0", "100") == -EIO);
        assert(writes == 2 && injected);
        assert(strstr(log_text, "Capture invalid"));
        if (fault == SELECTED_BIT) {
            assert(restore_pending && !(regs[3] & PAD_BIT));
            assert(strstr(log_text, "Restored CONFIG0C[5]=1: FAILED"));
            unsigned before = writes;
            assert(sample("0", "100") == -EBUSY && writes == before);
            fault = NONE;
            assert(restore_bit(NULL, now) && !restore_pending && (regs[3] & PAD_BIT));
            assert(sample("0", "100") == 0);
        } else {
            assert(regs[3] & PAD_BIT);
        }
        ++cases;
    }
}

static void test_separate_windows(void)
{
    fresh(true);
    stimulus[0] = (struct input_change){10, true};
    stimulus[1] = (struct input_change){20, false};
    stimulus[2] = (struct input_change){100, true};
    stimulus[3] = (struct input_change){150, false};
    stimulus[4] = (struct input_change){210, true};
    stimulus[5] = (struct input_change){220, false};
    stimulus_count = 6;
    char *args[] = {"compare", "100"};
    assert(cmd_compare(NULL, 2, args) == 0 && now == 400);
    assert(occurrences("Observed samples=100 high=10 low=90 rising=1 falling=1") == 2);
    assert(occurrences("Observed samples=100 high=50 low=50 rising=0 falling=1") == 1);
    assert(occurrences("Observed samples=100 high=0 low=100 rising=0 falling=0") == 1);
    assert(occurrences("Edges retained=2 dropped=0") == 2);
    assert(occurrences("Edges retained=1 dropped=0") == 1);
    assert(occurrences("Edges retained=0 dropped=0") == 1);
    assert(occurrences("edge t_ms=10 level=1 previous_sampled_run_ms=10") == 2);
    assert(!strstr(log_text, "nominal_samples"));
    ++cases;

    fresh(true);
    input_follows_config = check_restore_print_order = true;
    assert(sample("0", "100") == 0 && now == 200 && writes == 2);
    assert(strstr(log_text, "CONFIG write raw INPUT: before=0x00000020 bit5=1 t_ms=0; "
                           "after=0x00000000 bit5=0 t_ms=0"));
    assert(strstr(log_text, "Restoration (new time origin) raw INPUT: "
                           "before=0x00000000 bit5=0 t_ms=0; "
                           "after=0x00000020 bit5=1 t_ms=0"));
    assert(occurrences("Observed samples=100 high=0 low=100 rising=0 falling=0") == 1);
    assert(occurrences("Observed samples=100 high=100 low=0 rising=0 falling=0") == 1);
    ++cases;
}

int main(void)
{
    test_parsing();
    test_success_and_reset();
    test_failures();
    test_startup();
    test_trace_timing();
    test_trace_overflow();
    test_observation_endpoints();
    test_restoration_observation();
    test_separate_windows();
    printf("PASS: %u scenario groups (actual CONFIG0C C handlers; simulated platform)\n", cases);
    return 0;
}
"""


def main():
    source_dir = Path(__file__).resolve().parent / "src"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise SystemExit("CC must name a native C compiler")
    with tempfile.TemporaryDirectory(prefix="cwbeat-config0c-native-") as temporary:
        work = Path(temporary)
        include = work / "include"
        include.mkdir()
        (include / "stub.h").write_text(STUB)
        for name in HEADERS:
            header = include / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "stub.h"\n')
        harness = work / "check.c"
        harness.write_text(HARNESS)
        executable = work / "check"
        command = compiler + [
            "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-function", "-I", str(include), "-I", str(source_dir),
            str(harness), "-o", str(executable),
        ]
        subprocess.run(command, check=True, timeout=90)
        subprocess.run([str(executable)], check=True, timeout=20)


if __name__ == "__main__":
    main()
