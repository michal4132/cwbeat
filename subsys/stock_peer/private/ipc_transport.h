/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CWBEAT_IPC_TRANSPORT_H
#define CWBEAT_IPC_TRANSPORT_H
#include <stdbool.h>
#include <stdint.h>
#include "limits.h"

enum ipc_result {
	IPC_OK,
	IPC_AGAIN,
	IPC_INVALID,
	IPC_NO_MEMORY
};
struct ipc_message {
	uintptr_t allocation;
	uint16_t service, length;
	const volatile uint8_t *payload;
};
/* Nonblocking, single executor. receive OK lends one allocation until release
 * OK. AGAIN leaves ownership unchanged. send OK commits exactly once; the
 * caller must never resend it. INVALID is fatal. Handles are backend-owned. */
struct ipc_transport_ops {
	bool (*ready)(void *context);
	enum ipc_result (*attach)(void *context);
	enum ipc_result (*receive)(void *context, struct ipc_message *message);
	enum ipc_result (*send)(void *context, uint16_t service, const uint8_t *data,
				uint16_t length);
	enum ipc_result (*release)(void *context, struct ipc_message *message);
	enum ipc_result (*notify)(void *context, uint32_t notification); /* optional */
	uint32_t (*failure)(void *context); /* optional backend diagnostic */
};
struct ipc_transport {
	const struct ipc_transport_ops *ops;
	void *context;
};
bool ipc_transport_ready(struct ipc_transport *transport);
enum ipc_result ipc_transport_attach(struct ipc_transport *transport);
enum ipc_result ipc_transport_receive(struct ipc_transport *transport, struct ipc_message *message);
enum ipc_result ipc_transport_send(struct ipc_transport *transport, uint16_t service,
				   const uint8_t *payload, uint16_t length);
enum ipc_result ipc_transport_release(struct ipc_transport *transport, struct ipc_message *message);
enum ipc_result ipc_transport_notify(struct ipc_transport *transport, uint32_t notification);
uint32_t ipc_transport_failure(struct ipc_transport *transport);
#endif
