/* SPDX-License-Identifier: Apache-2.0 */
#include "arena.h"
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#define RUN_FLAGS_OFFSET            UINT32_C(0x0A)
#define RUN_MEDIA_CONNECTION_OFFSET UINT32_C(2)
#define RUN_MEDIA_PLAYING_OFFSET    UINT32_C(0x4C)
#define RUN_AUXILIARY_OFFSET        UINT32_C(7)
#define RUN_AUXILIARY_ENABLED       UINT8_C(1)
#define RUN_AUXILIARY_OVERRIDE      (UINT8_C(1) << 3)
#define SHARED_BASE                                                                                \
	DT_REG_ADDR(DT_PHANDLE(DT_COMPAT_GET_ANY_STATUS_OKAY(jbl_go4_stock_ipc), memory_region))
#define SHARED_SIZE                                                                                \
	DT_REG_SIZE(DT_PHANDLE(DT_COMPAT_GET_ANY_STATUS_OKAY(jbl_go4_stock_ipc), memory_region))
BUILD_ASSERT(SHARED_SIZE == 0x10000, "Stock shared arena ABI size");
#define ARENA_HEADER_SIZE     UINT32_C(0x14)
#define OBJECT_HEADER_SIZE    UINT32_C(0x10)
#define ARENA_MAGIC           UINT32_C(0x72616873)
#define ARENA_FORMAT_BYTE     UINT32_C(4)
#define ARENA_OBJECT_COUNT    UINT32_C(3)
#define OBJECT_ROLES          UINT32_C(0x0103) /* allowed=TOP|BT, attached=TOP */
#define RUNS_SIZE             UINT32_C(0x74)
#define DBIF_SIZE             UINT32_C(0x4C)
#define GLOB_SIZE             UINT32_C(0xA0)
#define RUNS_HEADER           (SHARED_BASE + ARENA_HEADER_SIZE)
#define RUNS_ADDRESS          (RUNS_HEADER + OBJECT_HEADER_SIZE)
#define DBIF_HEADER           (RUNS_ADDRESS + RUNS_SIZE)
#define DBIF_ADDRESS          (DBIF_HEADER + OBJECT_HEADER_SIZE)
#define GLOB_HEADER           (DBIF_ADDRESS + DBIF_SIZE)
#define GLOB_ADDRESS          (GLOB_HEADER + OBJECT_HEADER_SIZE)
#define NAME_RUNS             UINT32_C(0x736E7572)
#define NAME_DBIF             UINT32_C(0x66696264)
#define NAME_GLOB             UINT32_C(0x626F6C67)
#define DBIF_MARKER_OFFSET    UINT32_C(0x12)
#define DBIF_MARKER           UINT16_C(0xA5C3)
#define EVENT_MAGIC_OFFSET    UINT32_C(0x30)
#define EVENT_MAGIC           UINT32_C(0xEDCB1234)
#define BT_STATE_SHIFT        4u
#define BT_STATE_MASK         UINT32_C(3)
#define BT_STATE_ENTERED      UINT32_C(1)
#define BT_APP_READY_OFFSET   UINT32_C(0x0D)
#define RUN_STATE_OFFSET      UINT32_C(0)
#define RUN_WAKE_READY_OFFSET UINT32_C(9)
#define RUN_STATE_AWAKE       UINT32_C(1)
#define RUN_STATE_ACTIVE_MAX  UINT32_C(7)
#define LIVENESS_OFFSET       UINT32_C(0x84)
#define LIVENESS_MAGIC        UINT32_C(0x55AA6688)
#define LIVENESS_THRESHOLD    UINT32_C(5)

static uint8_t auxiliary_cache;
static volatile uint32_t *reg(uintptr_t address)
{
	return (volatile uint32_t *)address;
}

static void object(uintptr_t address, uint32_t next, uint32_t size, uint32_t name)
{
	volatile uint32_t *header = reg(address);
	header[0] = next;
	header[1] = size;
	header[2] = OBJECT_ROLES;
	header[3] = name;
}

bool ipc_arena_init(void)
{
	auxiliary_cache = 0;
	for (volatile uint32_t *word = reg(SHARED_BASE); word < reg(SHARED_BASE + SHARED_SIZE);
	     ++word) {
		*word = 0;
	}
	volatile uint32_t *arena = reg(SHARED_BASE);
	arena[0] = ARENA_MAGIC;
	arena[1] = ARENA_FORMAT_BYTE;
	arena[2] = ARENA_OBJECT_COUNT;
	arena[3] = SHARED_SIZE;
	object(RUNS_HEADER, DBIF_HEADER, RUNS_SIZE, NAME_RUNS);
	object(DBIF_HEADER, GLOB_HEADER, DBIF_SIZE, NAME_DBIF);
	object(GLOB_HEADER, 0, GLOB_SIZE, NAME_GLOB);
	*(volatile uint16_t *)(DBIF_ADDRESS + DBIF_MARKER_OFFSET) = DBIF_MARKER;
	*reg(GLOB_ADDRESS + EVENT_MAGIC_OFFSET) = EVENT_MAGIC;
	volatile uint32_t *liveness = reg(GLOB_ADDRESS + LIVENESS_OFFSET);
	liveness[0] = LIVENESS_MAGIC;
	liveness[1] = LIVENESS_THRESHOLD;
	return true;
}
uint8_t ipc_arena_run_flags(void)
{
	return *(volatile uint8_t *)(RUNS_ADDRESS + RUN_FLAGS_OFFSET);
}

struct peer_media_state ipc_arena_media_state(void)
{
	volatile uint8_t *runs = (volatile uint8_t *)RUNS_ADDRESS;
	uint8_t flags = runs[RUN_FLAGS_OFFSET];
	if ((flags & RUN_AUXILIARY_ENABLED) && !(flags & RUN_AUXILIARY_OVERRIDE)) {
		uint8_t auxiliary = runs[RUN_AUXILIARY_OFFSET];
		if (auxiliary) {
			auxiliary_cache = auxiliary;
		}
	} else {
		auxiliary_cache = 0;
	}
	return (struct peer_media_state){runs[RUN_MEDIA_CONNECTION_OFFSET],
					 runs[RUN_MEDIA_PLAYING_OFFSET], auxiliary_cache};
}

/* Exact stock TOP 0x4F4D2 predicate, read from our initialized shared objects.
 * Application initialization alone is readiness 1; wake must reach 2 or 3. */
uint8_t ipc_arena_bt_readiness(void)
{
	if (((*(volatile uint8_t *)GLOB_ADDRESS >> BT_STATE_SHIFT) & BT_STATE_MASK) !=
		    BT_STATE_ENTERED ||
	    !*(volatile uint8_t *)(RUNS_ADDRESS + BT_APP_READY_OFFSET)) {
		return 0;
	}
	uint8_t state = *(volatile uint8_t *)(RUNS_ADDRESS + RUN_STATE_OFFSET);
	if (!state) {
		return *(volatile uint8_t *)(RUNS_ADDRESS + RUN_WAKE_READY_OFFSET)
			       ? IPC_BT_READINESS_AWAKE
			       : IPC_BT_READINESS_INITIALIZED;
	}
	if (state == RUN_STATE_AWAKE) {
		return IPC_BT_READINESS_AWAKE;
	}
	return state <= RUN_STATE_ACTIVE_MAX ? IPC_BT_READINESS_ACTIVE
					     : IPC_BT_READINESS_UNAVAILABLE;
}

bool ipc_arena_bt_ready(void)
{
	return *(volatile uint8_t *)(RUNS_ADDRESS + BT_APP_READY_OFFSET) != 0;
}
