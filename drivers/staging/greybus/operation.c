/*
 * Greybus operations
 *
 * Copyright 2014 Google Inc.
 *
 * Released under the GPLv2 only.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#include "greybus.h"

/*
 * The top bit of the type in an operation message header indicates
 * whether the message is a request (bit clear) or response (bit set)
 */
#define GB_OPERATION_TYPE_RESPONSE	0x80

#define OPERATION_TIMEOUT_DEFAULT	1000	/* milliseconds */

/*
 * XXX This needs to be coordinated with host driver parameters
 * XXX May need to reduce to allow for message header within a page
 */
#define GB_OPERATION_MESSAGE_SIZE_MAX	4096

static struct kmem_cache *gb_operation_cache;

/* Workqueue to handle Greybus operation completions. */
static struct workqueue_struct *gb_operation_recv_workqueue;

/*
 * All operation messages (both requests and responses) begin with
 * a header that encodes the size of the data (header included).
 * This header also contains a unique identifier, which is used to
 * keep track of in-flight operations.  The header contains an
 * operation type field, whose interpretation is dependent on what
 * type of protocol is used over the connection.
 *
 * The high bit (0x80) of the operation type field is used to
 * indicate whether the message is a request (clear) or a response
 * (set).
 *
 * Response messages include an additional status byte, which
 * communicates the result of the corresponding request.  A zero
 * status value means the operation completed successfully.  Any
 * other value indicates an error; in this case, the payload of the
 * response message (if any) is ignored.  The status byte must be
 * zero in the header for a request message.
 *
 * The wire format for all numeric fields in the header is little
 * endian.  Any operation-specific data begins immediately after the
 * header, and is 64-bit aligned.
 */
struct gb_operation_msg_hdr {
	__le16	size;		/* Size in bytes of header + payload */
	__le16	operation_id;	/* Operation unique id */
	__u8	type;		/* E.g GB_I2C_TYPE_* or GB_GPIO_TYPE_* */
	__u8	result;		/* Result of request (in responses only) */
	/* 2 bytes pad, must be zero (ignore when read) */
} __aligned(sizeof(u64));

/* XXX Could be per-host device, per-module, or even per-connection */
static DEFINE_SPINLOCK(gb_operations_lock);

static void gb_pending_operation_insert(struct gb_operation *operation)
{
	struct gb_connection *connection = operation->connection;
	struct gb_operation_msg_hdr *header;

	/*
	 * Assign the operation's id and move it into its
	 * connection's pending list.
	 */
	spin_lock_irq(&gb_operations_lock);
	operation->id = ++connection->op_cycle;
	list_move_tail(&operation->links, &connection->pending);
	spin_unlock_irq(&gb_operations_lock);

	/* Store the operation id in the request header */
	header = operation->request.buffer;
	header->operation_id = cpu_to_le16(operation->id);
}

static void gb_pending_operation_remove(struct gb_operation *operation)
{
	struct gb_connection *connection = operation->connection;

	/* Take us off of the list of pending operations */
	spin_lock_irq(&gb_operations_lock);
	list_move_tail(&operation->links, &connection->operations);
	spin_unlock_irq(&gb_operations_lock);
}

static struct gb_operation *
gb_pending_operation_find(struct gb_connection *connection, u16 operation_id)
{
	struct gb_operation *operation;
	bool found = false;

	spin_lock_irq(&gb_operations_lock);
	list_for_each_entry(operation, &connection->pending, links)
		if (operation->id == operation_id) {
			found = true;
			break;
		}
	spin_unlock_irq(&gb_operations_lock);

	return found ? operation : NULL;
}

static int gb_message_send(struct gb_message *message, gfp_t gfp_mask)
{
	struct gb_connection *connection = message->operation->connection;
	u16 dest_cport_id = connection->interface_cport_id;
	int ret = 0;

	message->cookie = connection->hd->driver->buffer_send(connection->hd,
					dest_cport_id,
					message->buffer,
					message->buffer_size,
					gfp_mask);
	if (IS_ERR(message->cookie)) {
		ret = PTR_ERR(message->cookie);
		message->cookie = NULL;
	}
	return ret;
}

/*
 * Cancel a message whose buffer we have passed to the host device
 * layer to be sent.
 */
static void gb_message_cancel(struct gb_message *message)
{
	struct greybus_host_device *hd;

	if (!message->cookie)
		return;	/* Don't bother if the message isn't in flight */

	hd = message->operation->connection->hd;
	hd->driver->buffer_cancel(message->cookie);
}

/*
 * An operations's response message has arrived.  If no callback was
 * supplied it was submitted for asynchronous completion, so we notify
 * any waiters.  Otherwise we assume calling the completion is enough
 * and nobody else will be waiting.
 */
static void gb_operation_complete(struct gb_operation *operation)
{
	if (operation->callback)
		operation->callback(operation);
	else
		complete_all(&operation->completion);
}

/* Wait for a submitted operation to complete */
int gb_operation_wait(struct gb_operation *operation)
{
	int ret;

	ret = wait_for_completion_interruptible(&operation->completion);
	/* If interrupted, cancel the in-flight buffer */
	if (ret < 0)
		gb_message_cancel(&operation->request);
	return ret;

}

static void gb_operation_request_handle(struct gb_operation *operation)
{
	struct gb_protocol *protocol = operation->connection->protocol;
	struct gb_operation_msg_hdr *header;

	header = operation->request.buffer;

	/*
	 * If the protocol has no incoming request handler, report
	 * an error and mark the request bad.
	 */
	if (protocol->request_recv) {
		protocol->request_recv(header->type, operation);
		return;
	}

	gb_connection_err(operation->connection,
		"unexpected incoming request type 0x%02hhx\n", header->type);
	operation->result = GB_OP_PROTOCOL_BAD;
}

/*
 * Either this operation contains an incoming request, or its
 * response has arrived.  An incoming request will have a null
 * response buffer pointer (it is the responsibility of the request
 * handler to allocate and fill in the response buffer).
 */
static void gb_operation_recv_work(struct work_struct *recv_work)
{
	struct gb_operation *operation;
	bool incoming_request;

	operation = container_of(recv_work, struct gb_operation, recv_work);
	incoming_request = operation->response.buffer == NULL;
	if (incoming_request)
		gb_operation_request_handle(operation);
	gb_operation_complete(operation);
}

/*
 * Timeout call for the operation.
 *
 * If this fires, something went wrong, so mark the result as timed out, and
 * run the completion handler, which (hopefully) should clean up the operation
 * properly.
 */
static void operation_timeout(struct work_struct *work)
{
	struct gb_operation *operation;

	operation = container_of(work, struct gb_operation, timeout_work.work);
	pr_debug("%s: timeout!\n", __func__);

	operation->result = GB_OP_TIMEOUT;
	gb_operation_complete(operation);
}

/*
 * Allocate a buffer to be used for an operation request or response
 * message.  For outgoing messages, both types of message contain a
 * common header, which is filled in here.  Incoming requests or
 * responses also contain the same header, but there's no need to
 * initialize it here (it'll be overwritten by the incoming
 * message).
 */
static int gb_operation_message_init(struct gb_operation *operation,
					u8 type, size_t size,
					bool request, gfp_t gfp_flags)
{
	struct gb_connection *connection = operation->connection;
	struct greybus_host_device *hd = connection->hd;
	struct gb_message *message;
	struct gb_operation_msg_hdr *header;

	if (size > GB_OPERATION_MESSAGE_SIZE_MAX)
		return -E2BIG;
	size += sizeof(*header);

	if (request) {
		message = &operation->request;
	} else {
		message = &operation->response;
		type |= GB_OPERATION_TYPE_RESPONSE;
	}

	message->buffer = hd->driver->buffer_alloc(size, gfp_flags);
	if (!message->buffer)
		return -ENOMEM;
	message->buffer_size = size;

	/* Fill in the header structure */
	header = message->buffer;
	header->size = cpu_to_le16(size);
	header->operation_id = 0;	/* Filled in when submitted */
	header->type = type;

	message->payload = header + 1;
	message->operation = operation;

	return 0;
}

static void gb_operation_message_exit(struct gb_message *message)
{
	struct greybus_host_device *hd;

	hd = message->operation->connection->hd;
	hd->driver->buffer_free(message->buffer);

	message->operation = NULL;
	message->payload = NULL;
	message->buffer = NULL;
	message->buffer_size = 0;
}

/*
 * Create a Greybus operation to be sent over the given connection.
 * The request buffer will big enough for a payload of the given
 * size.  Outgoing requests must specify the size of the response
 * buffer size, which must be sufficient to hold all expected
 * response data.
 *
 * Incoming requests will supply a response size of 0, and in that
 * case no response buffer is allocated.  (A response always
 * includes a status byte, so 0 is not a valid size.)  Whatever
 * handles the operation request is responsible for allocating the
 * response buffer.
 *
 * Returns a pointer to the new operation or a null pointer if an
 * error occurs.
 */
static struct gb_operation *
gb_operation_create_common(struct gb_connection *connection, bool outgoing,
				u8 type, size_t request_size,
				size_t response_size)
{
	struct gb_operation *operation;
	gfp_t gfp_flags = response_size ? GFP_KERNEL : GFP_ATOMIC;
	int ret;

	operation = kmem_cache_zalloc(gb_operation_cache, gfp_flags);
	if (!operation)
		return NULL;
	operation->connection = connection;

	ret = gb_operation_message_init(operation, type, request_size,
						true, gfp_flags);
	if (ret)
		goto err_cache;

	if (outgoing) {
		ret = gb_operation_message_init(operation, type, response_size,
						false, GFP_KERNEL);
		if (ret)
			goto err_request;
	}

	INIT_WORK(&operation->recv_work, gb_operation_recv_work);
	operation->callback = NULL;	/* set at submit time */
	init_completion(&operation->completion);
	INIT_DELAYED_WORK(&operation->timeout_work, operation_timeout);
	kref_init(&operation->kref);

	spin_lock_irq(&gb_operations_lock);
	list_add_tail(&operation->links, &connection->operations);
	spin_unlock_irq(&gb_operations_lock);

	return operation;

err_request:
	gb_operation_message_exit(&operation->request);
err_cache:
	kmem_cache_free(gb_operation_cache, operation);

	return NULL;
}

struct gb_operation *gb_operation_create(struct gb_connection *connection,
					u8 type, size_t request_size,
					size_t response_size)
{
	return gb_operation_create_common(connection, true, type,
					request_size, response_size);
}

static struct gb_operation *
gb_operation_create_incoming(struct gb_connection *connection,
					u8 type, size_t request_size,
					size_t response_size)
{
	return gb_operation_create_common(connection, false, type,
					request_size, response_size);
}

/*
 * Destroy a previously created operation.
 */
static void _gb_operation_destroy(struct kref *kref)
{
	struct gb_operation *operation;

	operation = container_of(kref, struct gb_operation, kref);

	/* XXX Make sure it's not in flight */
	spin_lock_irq(&gb_operations_lock);
	list_del(&operation->links);
	spin_unlock_irq(&gb_operations_lock);

	gb_operation_message_exit(&operation->response);
	gb_operation_message_exit(&operation->request);

	kmem_cache_free(gb_operation_cache, operation);
}

void gb_operation_put(struct gb_operation *operation)
{
	if (!WARN_ON(!operation))
		kref_put(&operation->kref, _gb_operation_destroy);
}

/*
 * Send an operation request message.  The caller has filled in
 * any payload so the request message is ready to go.  If non-null,
 * the callback function supplied will be called when the response
 * message has arrived indicating the operation is complete.  A null
 * callback function is used for a synchronous request; return from
 * this function won't occur until the operation is complete (or an
 * interrupt occurs).
 */
int gb_operation_request_send(struct gb_operation *operation,
				gb_operation_callback callback)
{
	unsigned long timeout;
	int ret;

	if (operation->connection->state != GB_CONNECTION_STATE_ENABLED)
		return -ENOTCONN;

	/*
	 * XXX
	 * I think the order of operations is going to be
	 * significant, and if so, we may need a mutex to surround
	 * setting the operation id and submitting the buffer.
	 */
	operation->callback = callback;
	gb_pending_operation_insert(operation);
	ret = gb_message_send(&operation->request, GFP_KERNEL);
	if (ret)
		return ret;

	/* We impose a time limit for requests to complete.  */
	timeout = msecs_to_jiffies(OPERATION_TIMEOUT_DEFAULT);
	schedule_delayed_work(&operation->timeout_work, timeout);
	if (!callback)
		ret = gb_operation_wait(operation);

	return ret;
}

/*
 * Send a response for an incoming operation request.
 */
int gb_operation_response_send(struct gb_operation *operation)
{
	gb_operation_destroy(operation);

	return 0;
}

/*
 * We've received data on a connection, and it doesn't look like a
 * response, so we assume it's a request.
 *
 * This is called in interrupt context, so just copy the incoming
 * data into the request buffer and handle the rest via workqueue.
 */
void gb_connection_recv_request(struct gb_connection *connection,
	u16 operation_id, u8 type, void *data, size_t size)
{
	struct gb_operation *operation;

	operation = gb_operation_create_incoming(connection, type, size, 0);
	if (!operation) {
		gb_connection_err(connection, "can't create operation");
		return;		/* XXX Respond with pre-allocated ENOMEM */
	}
	operation->id = operation_id;
	memcpy(operation->request.buffer, data, size);

	/* The rest will be handled in work queue context */
	queue_work(gb_operation_recv_workqueue, &operation->recv_work);
}

/*
 * We've received data that appears to be an operation response
 * message.  Look up the operation, and record that we've received
 * its repsonse.
 *
 * This is called in interrupt context, so just copy the incoming
 * data into the response buffer and handle the rest via workqueue.
 */
static void gb_connection_recv_response(struct gb_connection *connection,
				u16 operation_id, void *data, size_t size)
{
	struct gb_operation *operation;
	struct gb_message *message;
	struct gb_operation_msg_hdr *header;

	operation = gb_pending_operation_find(connection, operation_id);
	if (!operation) {
		gb_connection_err(connection, "operation not found");
		return;
	}

	cancel_delayed_work(&operation->timeout_work);
	gb_pending_operation_remove(operation);

	message = &operation->response;
	if (size > message->buffer_size) {
		operation->result = GB_OP_OVERFLOW;
		gb_connection_err(connection, "recv buffer too small");
		return;		/* XXX Should still complete operation */
	}

	/* Hack the status from the buffer into the header */
	header = message->buffer;
	header->result = *(char *)message->payload;	/* Eeew. */
	operation->result = header->result;
	if (operation->result == GB_OP_SUCCESS)
		memcpy(message->buffer, data, size);

	/* The rest will be handled in work queue context */
	queue_work(gb_operation_recv_workqueue, &operation->recv_work);
}

/*
 * Handle data arriving on a connection.  As soon as we return the
 * supplied data buffer will be reused (so unless we do something
 * with, it's effectively dropped).
 */
void gb_connection_recv(struct gb_connection *connection,
				void *data, size_t size)
{
	struct gb_operation_msg_hdr *header;
	size_t msg_size;
	u16 operation_id;

	if (connection->state != GB_CONNECTION_STATE_ENABLED) {
		gb_connection_err(connection, "dropping %zu received bytes",
			size);
		return;
	}

	if (size < sizeof(*header)) {
		gb_connection_err(connection, "message too small");
		return;
	}

	header = data;
	msg_size = (size_t)le16_to_cpu(header->size);
	if (msg_size > size) {
		gb_connection_err(connection, "incomplete message");
		return;		/* XXX Should still complete operation */
	}

	operation_id = le16_to_cpu(header->operation_id);
	if (header->type & GB_OPERATION_TYPE_RESPONSE)
		gb_connection_recv_response(connection, operation_id,
						data, msg_size);
	else
		gb_connection_recv_request(connection, operation_id,
						header->type, data, msg_size);
}

/*
 * Cancel an operation.
 */
void gb_operation_cancel(struct gb_operation *operation)
{
	operation->canceled = true;
	gb_message_cancel(&operation->request);
	if (operation->response.buffer)
		gb_message_cancel(&operation->response);
}

int gb_operation_init(void)
{
	gb_operation_cache = kmem_cache_create("gb_operation_cache",
				sizeof(struct gb_operation), 0, 0, NULL);
	if (!gb_operation_cache)
		return -ENOMEM;

	gb_operation_recv_workqueue = alloc_workqueue("greybus_recv", 0, 1);
	if (!gb_operation_recv_workqueue) {
		kmem_cache_destroy(gb_operation_cache);
		gb_operation_cache = NULL;
		return -ENOMEM;
	}

	return 0;
}

void gb_operation_exit(void)
{
	destroy_workqueue(gb_operation_recv_workqueue);
	gb_operation_recv_workqueue = NULL;
	kmem_cache_destroy(gb_operation_cache);
	gb_operation_cache = NULL;
}
