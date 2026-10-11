/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_GO4_TRANSPORT_H
#define CWBEAT_GO4_TRANSPORT_H
#include "ipc_transport.h"
#define GO4_PACKET_BYTES 64u
enum ipc_failure {
	IPC_FAILURE_NONE,
	IPC_FAILURE_CONTEXT,
	IPC_FAILURE_ARENA,
	IPC_FAILURE_HEAP_VIEW,
	IPC_FAILURE_QUEUE,
	IPC_FAILURE_PAYLOAD,
	IPC_FAILURE_ALLOCATE,
	IPC_FAILURE_RELEASE,
	IPC_FAILURE_MAILBOX
};
struct go4_transport {
	uintptr_t glob, queues, pool;
	uint32_t failure;
};

extern const struct ipc_transport_ops go4_transport_ops;
#endif
