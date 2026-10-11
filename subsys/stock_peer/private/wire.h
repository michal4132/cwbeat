/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_IPC_WIRE_H
#define CWBEAT_IPC_WIRE_H
#include <stdbool.h>
#include <stdint.h>

/* Implemented by the selected peer protocol. Decoded data borrows payload. */
struct ipc_wire_frame {
	uint8_t command;
	bool reply;
	uint16_t transaction, length;
	const uint8_t *data;
};
struct ipc_wire_ops {
	bool (*decode)(const uint8_t *payload, uint16_t length, struct ipc_wire_frame *frame);
	bool (*encode)(const struct ipc_wire_frame *frame, uint8_t *out, uint16_t capacity,
		       uint16_t *length);
	bool (*method_supported)(uint16_t service, uint8_t command);
};

/* Optional RPC envelope codec: seven-bit command with a reply flag, opaque
 * byte, little-endian transaction and data length, data, then a trailing byte.
 * Encoding zeros the opaque/trailing bytes; decoding ignores them and borrows
 * the payload. Backends impose their own service and packet/reply limits, or
 * supply a different codec through ipc_wire_ops.
 */
#define IPC_WIRE_RPC_COMMAND_MAX    0x7fu
#define IPC_WIRE_RPC_OVERHEAD_BYTES 7u

bool ipc_wire_rpc_decode(const uint8_t *payload, uint16_t length, struct ipc_wire_frame *frame);
bool ipc_wire_rpc_encode(const struct ipc_wire_frame *frame, uint8_t *out, uint16_t capacity,
			 uint16_t *length);
#endif
