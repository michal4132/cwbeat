/* SPDX-License-Identifier: Apache-2.0 */
#include "ipc_transport.h"
bool ipc_transport_ready(struct ipc_transport *t)
{
	return t && t->ops && t->ops->ready && t->ops->attach && t->ops->send && t->ops->receive &&
	       t->ops->release && t->ops->ready(t->context);
}
enum ipc_result ipc_transport_attach(struct ipc_transport *t)
{
	return t && t->ops && t->ops->attach ? t->ops->attach(t->context) : IPC_INVALID;
}
enum ipc_result ipc_transport_receive(struct ipc_transport *t, struct ipc_message *m)
{
	return t && t->ops && t->ops->receive && m ? t->ops->receive(t->context, m) : IPC_INVALID;
}
enum ipc_result ipc_transport_send(struct ipc_transport *t, uint16_t service, const uint8_t *data,
				   uint16_t length)
{
	return t && t->ops && t->ops->send && length <= IPC_TRANSPORT_MAX_PAYLOAD &&
			       (!length || data)
		       ? t->ops->send(t->context, service, data, length)
		       : IPC_INVALID;
}
enum ipc_result ipc_transport_release(struct ipc_transport *t, struct ipc_message *m)
{
	return t && t->ops && t->ops->release && m ? t->ops->release(t->context, m) : IPC_INVALID;
}
enum ipc_result ipc_transport_notify(struct ipc_transport *t, uint32_t notification)
{
	return t && t->ops && t->ops->notify ? t->ops->notify(t->context, notification)
					     : IPC_INVALID;
}
uint32_t ipc_transport_failure(struct ipc_transport *t)
{
	return t && t->ops && t->ops->failure ? t->ops->failure(t->context) : 0;
}
