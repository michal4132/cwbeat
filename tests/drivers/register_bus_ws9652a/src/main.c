/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <zephyr/irq_offload.h>
#include <zephyr/sys/sys_io.h>

static uint32_t mock_read32(mem_addr_t address);
static void mock_write32(uint32_t value, mem_addr_t address);
#undef DT_INST_FOREACH_STATUS_OKAY
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#define sys_read32  mock_read32
#define sys_write32 mock_write32
#include "../../../../drivers/misc/ws9652a_bus.c"
#undef sys_read32
#undef sys_write32

static const struct ws9652a_bus_config config = {.gate = 0x424001ec};
static struct ws9652a_bus_data data;
static const struct device dev = {.name = "register-bus", .config = &config, .data = &data};
static unsigned reads, writes;
static uint32_t gate;
static int early_result, worker_result;
static struct k_thread worker;
K_THREAD_STACK_DEFINE(worker_stack, 1024);
K_SEM_DEFINE(entered, 0, 1);
K_SEM_DEFINE(finished, 0, 1);

static uint32_t mock_read32(mem_addr_t address)
{
	zassert_equal(address, config.gate);
	zassert_equal_ptr(data.lock.owner, k_current_get());
	++reads;
	return gate;
}

static void mock_write32(uint32_t value, mem_addr_t address)
{
	zassert_equal(address, config.gate);
	zassert_equal_ptr(data.lock.owner, k_current_get());
	++writes;
	gate = value;
}

static int early(void)
{
	ws9652a_bus_init(&dev);
	early_result = ws9652a_bus_lock(&dev, false);
	return 0;
}
SYS_INIT(early, PRE_KERNEL_2, 99);

static void reset(void *unused)
{
	ARG_UNUSED(unused);
	ws9652a_bus_init(&dev);
	reads = writes = gate = 0;
	worker_result = 1;
	k_sem_reset(&entered);
	k_sem_reset(&finished);
}

ZTEST(register_bus, test_init_starts_closed_and_no_implicit_gate_access)
{
	zassert_equal(early_result, -EAGAIN);
	zassert_equal(ws9652a_bus_lock(&dev, true), -EBUSY);
	zassert_ok(ws9652a_bus_lock(&dev, false));
	ws9652a_bus_unlock(&dev);
	zassert_equal(reads + writes, 0);
	zassert_is_null(data.lock.owner);
}

ZTEST(register_bus, test_recursive_admission_and_raw_gate_access)
{
	zassert_ok(ws9652a_bus_lock(&dev, false));
	ws9652a_bus_set_peers_held_locked(&dev, true);
	zassert_ok(ws9652a_bus_lock(&dev, true));
	zassert_equal(data.lock.lock_count, 2);
	ws9652a_bus_gate_set_locked(&dev, 1);
	zassert_equal(ws9652a_bus_gate_get_locked(&dev), 1);
	ws9652a_bus_gate_set_locked(&dev, 0);
	ws9652a_bus_set_peers_held_locked(&dev, false);
	zassert_equal(ws9652a_bus_lock(&dev, true), -EBUSY);
	zassert_equal(data.lock.lock_count, 2); /* failed recursive admission unwinds only itself */
	ws9652a_bus_unlock(&dev);
	ws9652a_bus_unlock(&dev);
	zassert_equal(reads, 1);
	zassert_equal(writes, 2);
	zassert_is_null(data.lock.owner);
}

static void from_isr(const void *unused)
{
	ARG_UNUSED(unused);
	zassert_equal(ws9652a_bus_lock(&dev, false), -EWOULDBLOCK);
	zassert_equal(ws9652a_bus_lock(&dev, true), -EWOULDBLOCK);
}

ZTEST(register_bus, test_isr_refuses_without_hardware_access)
{
	irq_offload(from_isr, NULL);
	zassert_equal(reads + writes, 0);
}

static void try_lock(void *require_held, void *unused1, void *unused2)
{
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	k_sem_give(&entered);
	worker_result = ws9652a_bus_lock(&dev, (uintptr_t)require_held != 0);
	if (!worker_result) {
		ws9652a_bus_unlock(&dev);
	}
	k_sem_give(&finished);
}

static void start_worker(bool require_held)
{
	k_thread_create(&worker, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack), try_lock,
			(void *)(uintptr_t)require_held, NULL, NULL, 1, 0, K_NO_WAIT);
	zassert_ok(k_sem_take(&entered, K_SECONDS(1)));
}

ZTEST(register_bus, test_contending_thread_times_out_and_owner_keeps_lock)
{
	zassert_ok(ws9652a_bus_lock(&dev, false));
	start_worker(false);
	zassert_ok(k_sem_take(&finished, K_SECONDS(1)));
	zassert_equal(worker_result, -EBUSY);
	zassert_equal_ptr(data.lock.owner, k_current_get());
	ws9652a_bus_unlock(&dev);
	zassert_ok(k_thread_join(&worker, K_SECONDS(1)));
	zassert_equal(reads + writes, 0);
}

ZTEST(register_bus, test_admission_is_checked_after_waiting_for_owner)
{
	zassert_ok(ws9652a_bus_lock(&dev, false));
	ws9652a_bus_set_peers_held_locked(&dev, true);
	start_worker(true);
	zassert_equal(k_sem_take(&finished, K_NO_WAIT), -EBUSY);
	/* This models a peer release under the same domain lock. A waiting
	 * transfer must observe closed admission after the release completes.
	 */
	ws9652a_bus_set_peers_held_locked(&dev, false);
	ws9652a_bus_unlock(&dev);
	zassert_ok(k_sem_take(&finished, K_SECONDS(1)));
	zassert_equal(worker_result, -EBUSY);
	zassert_ok(k_thread_join(&worker, K_SECONDS(1)));
	zassert_is_null(data.lock.owner);
}

ZTEST_SUITE(register_bus, NULL, NULL, reset, NULL, NULL);
