/* SPDX-License-Identifier: Apache-2.0 */
#include "wire.h"
#include <string.h>

#define RPC_HEADER_BYTES 6u
#define RPC_REPLY_BIT    0x80u

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
static void put16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}
bool ipc_wire_rpc_decode(const uint8_t *payload, uint16_t length, struct ipc_wire_frame *frame)
{
	if (!payload || !frame || length < IPC_WIRE_RPC_OVERHEAD_BYTES ||
	    get16(payload + 4) != length - IPC_WIRE_RPC_OVERHEAD_BYTES) {
		return false;
	}
	*frame = (struct ipc_wire_frame){.command = payload[0] & ~RPC_REPLY_BIT,
					 .reply = (payload[0] & RPC_REPLY_BIT) != 0,
					 .transaction = get16(payload + 2),
					 .length = get16(payload + 4),
					 .data = payload + RPC_HEADER_BYTES};
	/* Opaque and trailing bytes are not interpreted. */
	return true;
}
bool ipc_wire_rpc_encode(const struct ipc_wire_frame *frame, uint8_t *out, uint16_t capacity,
			 uint16_t *length)
{
	if (!frame || !out || !length || (frame->command & RPC_REPLY_BIT) ||
	    capacity < IPC_WIRE_RPC_OVERHEAD_BYTES ||
	    frame->length > capacity - IPC_WIRE_RPC_OVERHEAD_BYTES ||
	    (frame->length && !frame->data)) {
		return false;
	}
	out[0] = frame->command | (frame->reply ? RPC_REPLY_BIT : 0);
	out[1] = 0;
	put16(out + 2, frame->transaction);
	put16(out + 4, frame->length);
	if (frame->length) {
		memcpy(out + RPC_HEADER_BYTES, frame->data, frame->length);
	}
	out[RPC_HEADER_BYTES + frame->length] = 0;
	*length = IPC_WIRE_RPC_OVERHEAD_BYTES + frame->length;
	return true;
}
