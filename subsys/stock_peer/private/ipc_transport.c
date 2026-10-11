/* SPDX-License-Identifier: Apache-2.0 */
/* Shared ABI port for the hash-pinned Go 4 TOP/BT pair. No private stock code
 * addresses are called. Stock TOP reference: named 4A2C8, heap 5675C/524A4,
 * bin mapping 42B28, insert/remove 425AA/415C4, queues 45550/4557A.
 * First-fit chooses a physical free block, maintaining native TLSF bin metadata
 * so that unmodified BT may allocate or free either side's messages. */
#include "transport.h"
#include "stock.h"
#include <stdbool.h>
#include <stddef.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/irq.h>

#define IPC_NODE                DT_COMPAT_GET_ANY_STATUS_OKAY(jbl_go4_stock_ipc)
#define SHARED_BASE             DT_REG_ADDR(DT_PHANDLE(IPC_NODE, memory_region))
#define SHARED_BYTES            DT_REG_SIZE(DT_PHANDLE(IPC_NODE, memory_region))
#define SHARED_END              (SHARED_BASE + SHARED_BYTES)
#define ARENA_MAGIC             UINT32_C(0x72616873)
#define ARENA_FORMAT            UINT32_C(4)
#define MAX_OBJECTS             64u
#define ROLE_TOP                UINT32_C(1)
#define ROLE_BOTH               UINT32_C(3)
#define NAME_GLOB               UINT32_C(0x626F6C67)
#define NAME_QUEUES             UINT32_C(0x71637069)
#define NAME_POOL               UINT32_C(0x6C6F6F70)
#define GLOB_BYTES              UINT32_C(0xA0)
#define QUEUE_BYTES             UINT32_C(0x60)
#define POOL_BYTES              UINT32_C(0x2800)
#define GLOB_MAGIC_OFFSET       UINT32_C(0x30)
#define GLOB_MAGIC              UINT32_C(0xEDCB1234)
#define TX_EVENT_OFFSET         UINT32_C(0x34)
#define RX_EVENT_OFFSET         UINT32_C(0x48)
#define EVENT_MESSAGE           (UINT32_C(1) << 1)
#define EVENT_BT_WAKE_INDEX     7u
#define EVENT_BT_WAKE           (UINT32_C(1) << EVENT_BT_WAKE_INDEX)
#define EVENT_BT_SHUTDOWN_INDEX 8u
#define EVENT_BT_SHUTDOWN       (UINT32_C(1) << EVENT_BT_SHUTDOWN_INDEX)
#define TX_QUEUE_INDEX          0u
#define RX_QUEUE_INDEX          1u /* endpoint2 -> FIFO record1; selector [0,2,1,4,3,5] */
#define RX_QUEUE_LOCK_INDEX     2u /* Lock wrappers use the endpoint directly. */
#define HEAP_MAGIC              UINT32_C(0x2A59FA59)
#define HEAP_CONTROL_BYTES      UINT32_C(0x1F0)
#define HEAP_FIRST_BLOCK        UINT32_C(0x200)
#define HEAP_INITIAL_USED       UINT32_C(0x214)
#define HEAP_INITIAL_FREE       UINT32_C(0x25E0)
#define HEAP_ALIGNMENT          UINT32_C(8)
#define HEAP_FLAG_FREE          UINT32_C(1)
#define HEAP_FLAG_PREVIOUS_FREE UINT32_C(2)
#define HEAP_FLAG_MASK          UINT32_C(3)
#define HEAP_FIRST_LEVELS       13u
#define HEAP_SECOND_LEVELS      8u
#define HEAP_SMALL_LIMIT        UINT32_C(0x40)
#define HEAP_SMALL_SHIFT        3u
#define HEAP_FIRST_LEVEL_BIAS   5u
#define HEAP_MAX_BLOCKS         (POOL_BYTES / (2u * HEAP_ALIGNMENT))
#define NODE_LENGTH_BIAS        5u
#define NODE_OVERHEAD           16u
#define WORD_BYTES              4u
#define WORD_BITS               32u
#define HEADER_BYTES            8u
#define HALFWORD_BYTES          2u

static const struct mbox_dt_spec tx_mbox = MBOX_DT_SPEC_GET(IPC_NODE, tx);

struct arena {
	uint32_t magic, format, count, bytes, lock;
};
struct object {
	uint32_t next, size;
	uint8_t allowed, attached;
	uint16_t reserved;
	uint32_t name;
};
/* The lock at record N protects endpoint N, whose FIFO may be another record.
 */
struct queue {
	uint32_t lock, head, tail, count;
};
struct heap {
	uint32_t magic, reserved, used, peak, region, first_bitmap;
	uint32_t second_bitmap[HEAP_FIRST_LEVELS];
	uint32_t bins[HEAP_FIRST_LEVELS][HEAP_SECOND_LEVELS];
};
struct pool {
	uint32_t opaque, lock, information;
	struct heap heap;
};
struct block {
	uint32_t previous, size, free_previous, free_next;
};
struct node {
	uint32_t previous, next;
	uint16_t extent, service, length;
	uint8_t payload[];
};
struct event {
	uint32_t lock, words[4];
};
struct heap_view {
	volatile struct heap *heap;
	uintptr_t first, end;
};

_Static_assert(sizeof(uintptr_t) == WORD_BYTES, "IPC ABI requires 32-bit pointers");
_Static_assert(offsetof(struct node, payload) == NODE_OVERHEAD - HALFWORD_BYTES, "IPC node layout");
_Static_assert(sizeof(struct heap) <= HEAP_CONTROL_BYTES, "Native heap header size");

static volatile uint32_t *word(uintptr_t address)
{
	return (volatile uint32_t *)address;
}
static void barrier(void)
{
	__asm volatile("dmb" ::: "memory");
}
static uint32_t critical_enter(void)
{
	return irq_lock();
}
static void critical_leave(uint32_t previous)
{
	irq_unlock(previous);
}
/* Role0 uses byte0. The peer uses byte1. These are byte stores, never a
 * read/modify/write of the lock word that could clear the peer's claim. */
static bool lock(volatile uint32_t *address)
{
	if (*address) {
		return false;
	}
	*(volatile uint8_t *)address = ROLE_TOP;
	barrier();
	if ((*address & ~UINT32_C(0xFF)) == 0) {
		return true;
	}
	*(volatile uint8_t *)address = 0;
	barrier();
	return false;
}
static void unlock(volatile uint32_t *address)
{
	barrier();
	*(volatile uint8_t *)address = 0;
}
static bool extent(uintptr_t address, uint32_t bytes)
{
	return address >= SHARED_BASE && address <= SHARED_END && bytes <= SHARED_END - address;
}
static uint32_t aligned(uint32_t bytes, uint32_t alignment)
{
	return (bytes + alignment - 1u) & ~(alignment - 1u);
}
static uint32_t block_size(volatile struct block *block)
{
	return block->size & ~HEAP_FLAG_MASK;
}
static bool heap_view(struct go4_transport *transport, struct heap_view *view)
{
	volatile struct pool *pool = (volatile struct pool *)transport->pool;
	view->heap = &pool->heap;
	view->first = (uintptr_t)view->heap + HEAP_FIRST_BLOCK;
	uintptr_t descriptor = (uintptr_t)view->heap + HEAP_CONTROL_BYTES + HEADER_BYTES;
	if (view->heap->magic != HEAP_MAGIC || view->heap->region != descriptor ||
	    *word(descriptor + WORD_BYTES) != 0) {
		return false;
	}
	view->end = *word(descriptor);
	return view->end >= view->first &&
	       view->end <= transport->pool + POOL_BYTES - HEADER_BYTES &&
	       ((view->end - view->first) & (HEAP_ALIGNMENT - 1u)) == 0 &&
	       view->heap->used >= HEAP_INITIAL_USED && view->heap->used <= POOL_BYTES &&
	       view->heap->peak >= view->heap->used && view->heap->peak <= POOL_BYTES;
}
static bool block_extent(struct heap_view *view, uintptr_t address)
{
	return address >= view->first && address < view->end &&
	       ((address - view->first) & (HEAP_ALIGNMENT - 1u)) == 0 &&
	       sizeof(struct block) <= view->end + HEADER_BYTES - address;
}
static void size_class(uint32_t size, uint32_t *first, uint32_t *second)
{
	if (size < HEAP_SMALL_LIMIT) {
		*first = 0;
		*second = size >> HEAP_SMALL_SHIFT;
	} else {
		uint32_t logarithm = WORD_BITS - 1u - (uint32_t)__builtin_clz(size);
		*first = logarithm - HEAP_FIRST_LEVEL_BIAS;
		*second = (size >> (logarithm - HEAP_SMALL_SHIFT)) - HEAP_SECOND_LEVELS;
	}
}
/* Validate the physical chain under the pool lock, including previous-free
 * boundary tags. Returns the target allocation or a first-fit free block. */
static bool find_block(struct heap_view *view, uintptr_t target, uint32_t needed,
		       volatile struct block **found)
{
	uintptr_t address = view->first, previous = 0;
	bool previous_free = false;
	*found = NULL;
	for (uint32_t count = 0; count < HEAP_MAX_BLOCKS; ++count) {
		if (address == view->end) {
			volatile struct block *sentinel = (volatile struct block *)address;
			return block_size(sentinel) == 0 && !(sentinel->size & HEAP_FLAG_FREE) &&
			       ((sentinel->size & HEAP_FLAG_PREVIOUS_FREE) != 0) == previous_free &&
			       (!previous_free || sentinel->previous == previous);
		}
		if (!block_extent(view, address)) {
			return false;
		}
		volatile struct block *current = (volatile struct block *)address;
		uint32_t size = block_size(current);
		if (size < HEAP_ALIGNMENT || (size & (HEAP_ALIGNMENT - 1u)) ||
		    size > view->end - address - HEADER_BYTES ||
		    ((current->size & HEAP_FLAG_PREVIOUS_FREE) != 0) != previous_free ||
		    (previous_free && current->previous != previous)) {
			return false;
		}
		bool available = (current->size & HEAP_FLAG_FREE) != 0;
		if (address == target || (!target && !*found && available && size >= needed)) {
			*found = current;
		}
		previous = address;
		previous_free = available;
		address += size + HEADER_BYTES;
	}
	return false;
}
static bool bin_valid(struct heap_view *view, volatile struct block *block)
{
	uint32_t first, second;
	size_class(block_size(block), &first, &second);
	if (!(block->size & HEAP_FLAG_FREE) || first >= HEAP_FIRST_LEVELS ||
	    second >= HEAP_SECOND_LEVELS) {
		return false;
	}
	uintptr_t previous = block->free_previous, next = block->free_next;
	if (previous == (uintptr_t)block || next == (uintptr_t)block ||
	    (previous && previous == next)) {
		return false;
	}
	if (previous && (!block_extent(view, previous) ||
			 ((volatile struct block *)previous)->free_next != (uintptr_t)block)) {
		return false;
	}
	if (next && (!block_extent(view, next) ||
		     ((volatile struct block *)next)->free_previous != (uintptr_t)block)) {
		return false;
	}
	return (previous || view->heap->bins[first][second] == (uintptr_t)block) &&
	       (view->heap->first_bitmap & (UINT32_C(1) << first)) &&
	       (view->heap->second_bitmap[first] & (UINT32_C(1) << second));
}
static void bin_remove(struct heap_view *view, volatile struct block *block)
{
	uint32_t first, second;
	size_class(block_size(block), &first, &second);
	uintptr_t previous = block->free_previous, next = block->free_next;
	if (previous) {
		((volatile struct block *)previous)->free_next = next;
	}
	if (next) {
		((volatile struct block *)next)->free_previous = previous;
	}
	if (view->heap->bins[first][second] == (uintptr_t)block) {
		view->heap->bins[first][second] = next;
		if (!next) {
			view->heap->second_bitmap[first] &= ~(UINT32_C(1) << second);
			if (!view->heap->second_bitmap[first]) {
				view->heap->first_bitmap &= ~(UINT32_C(1) << first);
			}
		}
	}
	block->free_previous = block->free_next = 0;
}
static bool bin_head_valid(struct heap_view *view, uint32_t size)
{
	uint32_t first, second;
	size_class(size, &first, &second);
	if (first >= HEAP_FIRST_LEVELS || second >= HEAP_SECOND_LEVELS) {
		return false;
	}
	uintptr_t head = view->heap->bins[first][second];
	if (!head) {
		return true;
	}
	if (!block_extent(view, head)) {
		return false;
	}
	volatile struct block *block = (volatile struct block *)head;
	uint32_t actual_first, actual_second;
	size_class(block_size(block), &actual_first, &actual_second);
	return first == actual_first && second == actual_second && block->free_previous == 0 &&
	       bin_valid(view, block);
}
static void bin_insert(struct heap_view *view, volatile struct block *block)
{
	uint32_t first, second;
	size_class(block_size(block), &first, &second);
	uintptr_t head = view->heap->bins[first][second];
	block->free_previous = 0;
	block->free_next = head;
	if (head) {
		((volatile struct block *)head)->free_previous = (uintptr_t)block;
	}
	view->heap->bins[first][second] = (uintptr_t)block;
	view->heap->second_bitmap[first] |= UINT32_C(1) << second;
	view->heap->first_bitmap |= UINT32_C(1) << first;
}
static enum ipc_result heap_allocate(struct heap_view *view, uint32_t requested,
				     uintptr_t *allocation)
{
	uint32_t needed =
		aligned(requested < HEAP_ALIGNMENT ? HEAP_ALIGNMENT : requested, HEAP_ALIGNMENT);
	volatile struct block *block;
	if (!find_block(view, 0, needed, &block)) {
		return IPC_INVALID;
	}
	if (!block) {
		return IPC_NO_MEMORY;
	}
	uint32_t original = block_size(block), remaining = original - needed;
	bool split = remaining >= sizeof(struct block);
	if (!bin_valid(view, block) || (split && !bin_head_valid(view, remaining - HEADER_BYTES))) {
		return IPC_INVALID;
	}
	bin_remove(view, block);
	volatile struct block *next =
		(volatile struct block *)((uintptr_t)block + original + HEADER_BYTES);
	if (split) {
		volatile struct block *remainder =
			(volatile struct block *)((uintptr_t)block + needed + HEADER_BYTES);
		remainder->size = (remaining - HEADER_BYTES) | HEAP_FLAG_FREE;
		next->previous = (uintptr_t)remainder;
		bin_insert(view, remainder);
		block->size = needed | (block->size & HEAP_FLAG_PREVIOUS_FREE);
	} else {
		block->size &= ~HEAP_FLAG_FREE;
		next->size &= ~HEAP_FLAG_PREVIOUS_FREE;
	}
	uint32_t used = view->heap->used + block_size(block) + HEADER_BYTES;
	view->heap->used = used;
	if (used > view->heap->peak) {
		view->heap->peak = used;
	}
	*allocation = (uintptr_t)block + HEADER_BYTES;
	return IPC_OK;
}
static enum ipc_result heap_release(struct heap_view *view, uintptr_t allocation)
{
	volatile struct block *block;
	if (allocation < HEADER_BYTES || !find_block(view, allocation - HEADER_BYTES, 0, &block) ||
	    !block || (block->size & HEAP_FLAG_FREE)) {
		return IPC_INVALID;
	}
	uint32_t released = block_size(block) + HEADER_BYTES;
	volatile struct block *next = (volatile struct block *)((uintptr_t)block + released);
	volatile struct block *previous =
		(block->size & HEAP_FLAG_PREVIOUS_FREE)
			? (volatile struct block *)(uintptr_t)block->previous
			: NULL;
	bool next_free = (next->size & HEAP_FLAG_FREE) != 0;
	uint32_t merged = block_size(block);
	if (next_free) {
		if (!bin_valid(view, next)) {
			return IPC_INVALID;
		}
		merged += block_size(next) + HEADER_BYTES;
	}
	if (previous) {
		if (!bin_valid(view, previous)) {
			return IPC_INVALID;
		}
		merged += block_size(previous) + HEADER_BYTES;
	}
	if (view->heap->used < HEAP_INITIAL_USED + released || !bin_head_valid(view, merged)) {
		return IPC_INVALID;
	}
	if (next_free) {
		bin_remove(view, next);
	}
	if (previous) {
		bin_remove(view, previous);
		block = previous;
	}
	block->size = merged | (block->size & HEAP_FLAG_PREVIOUS_FREE) | HEAP_FLAG_FREE;
	bin_insert(view, block);
	next = (volatile struct block *)((uintptr_t)block + merged + HEADER_BYTES);
	next->previous = (uintptr_t)block;
	next->size |= HEAP_FLAG_PREVIOUS_FREE;
	view->heap->used -= released;
	return IPC_OK;
}
static bool context_valid(struct go4_transport *transport)
{
	return extent(transport->glob, GLOB_BYTES) && extent(transport->queues, QUEUE_BYTES) &&
	       extent(transport->pool, POOL_BYTES) &&
	       ((transport->glob | transport->queues | transport->pool) & (WORD_BYTES - 1u)) == 0 &&
	       *word(transport->glob + GLOB_MAGIC_OFFSET) == GLOB_MAGIC;
}

static enum ipc_result go4_attach(void *context)
{
	struct go4_transport *transport = context;
	transport->failure = IPC_FAILURE_NONE;
	if (!mbox_is_ready_dt(&tx_mbox)) {
		transport->failure = IPC_FAILURE_MAILBOX;
		return IPC_INVALID;
	}
	volatile struct arena *arena = (volatile struct arena *)SHARED_BASE;
	if (arena->magic != ARENA_MAGIC) {
		return IPC_AGAIN;
	}
	uint32_t saved = critical_enter();
	if (!lock(&arena->lock)) {
		critical_leave(saved);
		return IPC_AGAIN;
	}
	enum ipc_result result = IPC_INVALID;
	volatile struct object *glob = NULL, *queues = NULL, *pool = NULL;
	uintptr_t address = SHARED_BASE + sizeof(struct arena);
	if (arena->format != ARENA_FORMAT || arena->bytes != SHARED_BYTES ||
	    arena->count > MAX_OBJECTS) {
		goto done;
	}
	for (uint32_t index = 0; index < arena->count; ++index) {
		if (!extent(address, sizeof(struct object)) || (address & (WORD_BYTES - 1u))) {
			goto done;
		}
		volatile struct object *object = (volatile struct object *)address;
		uintptr_t payload = address + sizeof(struct object);
		if (!extent(payload, object->size) || !object->allowed ||
		    (object->allowed & ~ROLE_BOTH) || (object->attached & ~object->allowed)) {
			goto done;
		}
		volatile struct object **slot = NULL;
		uint32_t required = 0;
		if (object->name == NAME_GLOB) {
			slot = &glob;
			required = GLOB_BYTES;
		}
		if (object->name == NAME_QUEUES) {
			slot = &queues;
			required = QUEUE_BYTES;
		}
		if (object->name == NAME_POOL) {
			slot = &pool;
			required = POOL_BYTES;
		}
		if (slot) {
			if (*slot || object->size != required || object->allowed != ROLE_BOTH) {
				goto done;
			}
			*slot = object;
		}
		uintptr_t expected = index + 1u == arena->count
					     ? 0
					     : payload + aligned(object->size, WORD_BYTES);
		if (object->next != expected) {
			goto done;
		}
		address = object->next;
	}
	result = IPC_AGAIN;
	if (!glob || !queues || !pool) {
		goto done;
	}
	struct go4_transport candidate = {(uintptr_t)(glob + 1), (uintptr_t)(queues + 1),
					  (uintptr_t)(pool + 1), IPC_FAILURE_NONE};
	/* BT B1E3C writes information only after its initialization and unlock.
	 * The native initializer publishes heap magic before its final peak
	 * store; magic alone is not the completion marker for the transport
	 * object. */
	volatile struct pool *ready_pool = (volatile struct pool *)candidate.pool;
	if (!context_valid(&candidate) || ready_pool->information != HEAP_INITIAL_FREE ||
	    ready_pool->heap.magic != HEAP_MAGIC) {
		goto done;
	}
	glob->attached |= ROLE_TOP;
	queues->attached |= ROLE_TOP;
	pool->attached |= ROLE_TOP;
	transport->glob = candidate.glob;
	transport->queues = candidate.queues;
	transport->pool = candidate.pool;
	result = IPC_OK;
done:
	if (result == IPC_INVALID) {
		transport->failure = IPC_FAILURE_ARENA;
	}
	unlock(&arena->lock);
	critical_leave(saved);
	return result;
}
static bool queue_valid(struct heap_view *view, volatile struct queue *queue)
{
	if (!queue->count) {
		return !queue->head && !queue->tail;
	}
	if (queue->count > HEAP_MAX_BLOCKS ||
	    ((queue->count == 1u) != (queue->head == queue->tail))) {
		return false;
	}
	volatile struct block *head, *tail;
	if (!find_block(view, queue->head - HEADER_BYTES, 0, &head) || !head ||
	    !find_block(view, queue->tail - HEADER_BYTES, 0, &tail) || !tail ||
	    (head->size & HEAP_FLAG_FREE) || (tail->size & HEAP_FLAG_FREE)) {
		return false;
	}
	uintptr_t address = queue->head, previous = 0;
	for (uint32_t index = 0; index < queue->count; ++index) {
		if (!block_extent(view, address - HEADER_BYTES)) {
			return false;
		}
		volatile struct node *node = (volatile struct node *)address;
		volatile struct block *block = (volatile struct block *)(address - HEADER_BYTES);
		if ((block->size & HEAP_FLAG_FREE) || block_size(block) < NODE_OVERHEAD ||
		    block_size(block) > view->end - address ||
		    (index && node->previous != previous)) {
			return false;
		}
		previous = address;
		address = node->next;
	}
	return !address && previous == queue->tail;
}

static enum ipc_result go4_receive(void *context, struct ipc_message *message)
{
	struct go4_transport *transport = context;
	transport->failure = IPC_FAILURE_NONE;
	if (!context_valid(transport) || message->allocation) {
		transport->failure = IPC_FAILURE_CONTEXT;
		return IPC_INVALID;
	}
	volatile struct queue *queue = (volatile struct queue *)transport->queues + RX_QUEUE_INDEX;
	volatile uint32_t *queue_lock =
		&((volatile struct queue *)transport->queues)[RX_QUEUE_LOCK_INDEX].lock;
	volatile struct event *event = (volatile struct event *)(transport->glob + RX_EVENT_OFFSET);
	volatile struct pool *pool = (volatile struct pool *)transport->pool;
	uint32_t saved = critical_enter();
	if (!lock(&event->lock)) {
		critical_leave(saved);
		return IPC_AGAIN;
	}
	if (!lock(queue_lock)) {
		unlock(&event->lock);
		critical_leave(saved);
		return IPC_AGAIN;
	}
	/* Empty FIFO inspection needs no heap metadata and must not race its
	 * publication. Validate both endpoints; never excuse a malformed FIFO.
	 */
	if (!queue->count) {
		enum ipc_result empty = queue->head || queue->tail ? IPC_INVALID : IPC_AGAIN;
		if (empty == IPC_INVALID) {
			transport->failure = IPC_FAILURE_QUEUE;
		} else {
			event->words[0] &= ~EVENT_MESSAGE;
		}
		unlock(queue_lock);
		unlock(&event->lock);
		critical_leave(saved);
		return empty;
	}
	if (!lock(&pool->lock)) {
		unlock(queue_lock);
		unlock(&event->lock);
		critical_leave(saved);
		return IPC_AGAIN;
	}
	struct heap_view view;
	enum ipc_result result = IPC_INVALID;
	transport->failure = IPC_FAILURE_HEAP_VIEW;
	if (!heap_view(transport, &view)) {
		goto done;
	}
	transport->failure = IPC_FAILURE_QUEUE;
	if (!queue_valid(&view, queue)) {
		goto done;
	}
	volatile struct node *node = (volatile struct node *)(uintptr_t)queue->head;
	volatile struct block *block;
	uint32_t length = node->length;
	transport->failure = IPC_FAILURE_PAYLOAD;
	if (!find_block(&view, queue->head - HEADER_BYTES, 0, &block) || !block ||
	    node->service >= GO4_SERVICE_LIMIT || length > GO4_PACKET_BYTES ||
	    node->extent != length + NODE_LENGTH_BIAS || (block->size & HEAP_FLAG_FREE) ||
	    block_size(block) < length + NODE_OVERHEAD ||
	    !extent((uintptr_t)node, block_size(block)) ||
	    ((queue->count == 1u) != (node->next == 0)) ||
	    (node->next &&
	     (!block_extent(&view, node->next - HEADER_BYTES) ||
	      ((volatile struct node *)(uintptr_t)node->next)->previous != (uintptr_t)node))) {
		goto done;
	}
	message->allocation = (uintptr_t)node;
	message->service = node->service;
	message->length = length;
	message->payload = node->payload;
	queue->head = node->next;
	if (!--queue->count) {
		queue->tail = 0;
		event->words[0] &= ~EVENT_MESSAGE;
	}
	result = IPC_OK;
	transport->failure = IPC_FAILURE_NONE;
done:
	unlock(&pool->lock);
	unlock(queue_lock);
	unlock(&event->lock);
	critical_leave(saved);
	return result;
}

static enum ipc_result go4_send(void *context, uint16_t service, const uint8_t *payload,
				uint16_t length)
{
	struct go4_transport *transport = context;
	transport->failure = IPC_FAILURE_NONE;
	if (!context_valid(transport) || service >= GO4_SERVICE_LIMIT ||
	    length > GO4_PACKET_BYTES || (length && !payload)) {
		transport->failure = IPC_FAILURE_CONTEXT;
		return IPC_INVALID;
	}
	volatile struct queue *queue = (volatile struct queue *)transport->queues + TX_QUEUE_INDEX;
	volatile struct event *event = (volatile struct event *)(transport->glob + TX_EVENT_OFFSET);
	volatile struct pool *pool = (volatile struct pool *)transport->pool;
	uint32_t saved = critical_enter();
	if (!lock(&event->lock)) {
		critical_leave(saved);
		return IPC_AGAIN;
	}
	if (!lock(&queue->lock)) {
		unlock(&event->lock);
		critical_leave(saved);
		return IPC_AGAIN;
	}
	if (!lock(&pool->lock)) {
		unlock(&queue->lock);
		unlock(&event->lock);
		critical_leave(saved);
		return IPC_AGAIN;
	}
	struct heap_view view;
	uintptr_t allocation;
	enum ipc_result result = IPC_INVALID;
	transport->failure = IPC_FAILURE_HEAP_VIEW;
	if (!heap_view(transport, &view)) {
		goto done;
	}
	transport->failure = IPC_FAILURE_QUEUE;
	if (!queue_valid(&view, queue) ||
	    (queue->tail && ((volatile struct node *)(uintptr_t)queue->tail)->next)) {
		goto done;
	}
	result = heap_allocate(&view, length + NODE_OVERHEAD, &allocation);
	transport->failure = result == IPC_INVALID ? IPC_FAILURE_ALLOCATE : IPC_FAILURE_NONE;
	if (result != IPC_OK) {
		goto done;
	}
	volatile struct node *node = (volatile struct node *)allocation;
	node->previous = queue->tail;
	node->next = 0;
	node->extent = length + NODE_LENGTH_BIAS;
	node->service = service;
	node->length = length;
	for (uint32_t index = 0; index < length; ++index) {
		node->payload[index] = payload[index];
	}
	if (queue->tail) {
		((volatile struct node *)(uintptr_t)queue->tail)->next = allocation;
	} else {
		queue->head = allocation;
	}
	queue->tail = allocation;
	++queue->count;
	event->words[0] |= EVENT_MESSAGE;
	barrier();
	if (mbox_send_dt(&tx_mbox, NULL)) {
		transport->failure = IPC_FAILURE_MAILBOX;
		result = IPC_INVALID;
	}
done:
	unlock(&pool->lock);
	unlock(&queue->lock);
	unlock(&event->lock);
	critical_leave(saved);
	return result;
}

static enum ipc_result go4_release(void *context, struct ipc_message *message)
{
	struct go4_transport *transport = context;
	transport->failure = IPC_FAILURE_NONE;
	if (!context_valid(transport) || !message->allocation) {
		transport->failure = IPC_FAILURE_CONTEXT;
		return IPC_INVALID;
	}
	volatile struct pool *pool = (volatile struct pool *)transport->pool;
	uint32_t saved = critical_enter();
	if (!lock(&pool->lock)) {
		critical_leave(saved);
		return IPC_AGAIN;
	}
	struct heap_view view;
	enum ipc_result result = IPC_INVALID;
	transport->failure = IPC_FAILURE_HEAP_VIEW;
	if (heap_view(transport, &view)) {
		result = heap_release(&view, message->allocation);
		transport->failure = result == IPC_INVALID ? IPC_FAILURE_RELEASE : IPC_FAILURE_NONE;
	}
	if (result == IPC_OK) {
		message->allocation = 0;
		message->payload = NULL;
		message->length = 0;
	}
	unlock(&pool->lock);
	critical_leave(saved);
	return result;
}

static enum ipc_result request_bt_event(struct go4_transport *transport, uint32_t mask)
{
	transport->failure = IPC_FAILURE_NONE;
	if (!context_valid(transport)) {
		transport->failure = IPC_FAILURE_CONTEXT;
		return IPC_INVALID;
	}
	volatile struct event *event = (volatile struct event *)(transport->glob + TX_EVENT_OFFSET);
	uint32_t saved = critical_enter();
	if (!lock(&event->lock)) {
		critical_leave(saved);
		return IPC_AGAIN;
	}
	event->words[0] |= mask;
	/* Match 0x453FC: publish/unlock and restore the caller's interrupt
	 * state before ringing BT. No FIFO or shared allocation participates.
	 */
	unlock(&event->lock);
	critical_leave(saved);
	if (mbox_send_dt(&tx_mbox, NULL)) {
		transport->failure = IPC_FAILURE_MAILBOX;
		return IPC_INVALID;
	}
	return IPC_OK;
}

static enum ipc_result go4_notify(void *context, uint32_t notification)
{
	if (notification == 7) {
		return request_bt_event(context, EVENT_BT_WAKE);
	}
	if (notification == 8) {
		return request_bt_event(context, EVENT_BT_SHUTDOWN);
	}
	return IPC_INVALID;
}
static bool go4_ready(void *context)
{
	(void)context;
	return mbox_is_ready_dt(&tx_mbox);
}
static uint32_t go4_failure(void *context)
{
	return ((struct go4_transport *)context)->failure;
}
const struct ipc_transport_ops go4_transport_ops = {.ready = go4_ready,
						    .attach = go4_attach,
						    .receive = go4_receive,
						    .send = go4_send,
						    .release = go4_release,
						    .notify = go4_notify,
						    .failure = go4_failure};
