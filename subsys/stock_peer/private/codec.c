/* SPDX-License-Identifier: Apache-2.0 */
#include "backend.h"
#include "stock.h"
#include "transport.h"

#define GO4_REPLY_BYTES 48u

static bool go4_wire_method_supported(uint16_t service, uint8_t command)
{
	return service < GO4_SERVICE_LIMIT && service != GO4_POWER_RAW_TYPE &&
	       command <= IPC_WIRE_RPC_COMMAND_MAX;
}

static bool go4_wire_decode(const uint8_t *payload, uint16_t length, struct ipc_wire_frame *frame)
{
	return length <= GO4_PACKET_BYTES && ipc_wire_rpc_decode(payload, length, frame);
}

static bool go4_wire_encode(const struct ipc_wire_frame *frame, uint8_t *out, uint16_t capacity,
			    uint16_t *length)
{
	if (!frame || frame->length > GO4_PACKET_BYTES - IPC_WIRE_RPC_OVERHEAD_BYTES ||
	    (frame->reply && frame->length > GO4_REPLY_BYTES)) {
		return false;
	}
	return ipc_wire_rpc_encode(frame, out, capacity, length);
}

const struct ipc_wire_ops go4_wire_ops = {.encode = go4_wire_encode,
					  .decode = go4_wire_decode,
					  .method_supported = go4_wire_method_supported};
