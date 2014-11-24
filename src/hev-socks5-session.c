/*
 ============================================================================
 Name        : hev-socks5-session.c
 Author      : Heiher <r@hev.cc>
 Copyright   : Copyright (c) 2014 everyone.
 Description : Socks5 session
 ============================================================================
 */

#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "hev-buffer.h"
#include "hev-socket.h"
#include "hev-pollable-fd.h"
#include "hev-socks5-proto.h"
#include "hev-dns-resolver.h"
#include "hev-socks5-session.h"

struct _HevSocks5Session
{
	int client_fd;
	int remote_fd;
	bool is_idle;

	HevBuffer *buffer0;
	HevBuffer *buffer1;
	HevSocket *socket;
	HevDNSResolver *resolver;
	HevPollableFD *client_pfd;
	HevPollableFD *remote_pfd;
	HevBufferList *buffer_list;

	struct {
		HevSocks5SessionCloseNotify notifer;
		void *data;
	} notify;

	struct sockaddr_in addr;
};

static bool hev_socks5_session_client_read (HevSocks5Session *self, HevBuffer *buffer,
			HevPollableFDReadyCallback callback);
static bool hev_socks5_session_remote_read (HevSocks5Session *self, HevBuffer *buffer,
			HevPollableFDReadyCallback callback);
static bool hev_socks5_session_socket_connect (HevSocks5Session *self);

static ssize_t sock_reader (int fd, void *buf, size_t count, void *user_data);
static ssize_t sock_writer (int fd, void *buf, size_t count, void *user_data);

static void read_auth_req_handler (HevPollableFD *fd, void *user_data);
static void write_auth_res_handler (HevPollableFD *fd, void *user_data);
static void read_req_handler (HevPollableFD *fd, void *user_data);
static void resolver_handler (HevDNSResolver *resolver, void *user_data);
static void socket_connect_handler (HevSocket *socket, void *user_data);
static void write_res_handler (HevPollableFD *fd, void *user_data);
static void read_client_data_handler (HevPollableFD *fd, void *user_data);
static void read_remote_data_handler (HevPollableFD *fd, void *user_data);
static void write_client_data_handler (HevPollableFD *fd, void *user_data);
static void write_remote_data_handler (HevPollableFD *fd, void *user_data);

HevSocks5Session *
hev_socks5_session_new (int fd, HevBufferList *buffer_list,
			HevSocks5SessionCloseNotify notify, void *notify_data)
{
	HevSocks5Session *self;
	int left_size;

	self = hev_malloc0 (sizeof (HevSocks5Session));
	if (!self) {
		fprintf (stderr, "Malloc HevSocks5Session failed!\n");
		return NULL;
	}

	self->client_fd = fd;
	self->is_idle = false;
	self->buffer_list = buffer_list;
	self->notify.notifer = notify;
	self->notify.data = notify_data;

	self->client_pfd = hev_pollable_fd_new (fd, 1);
	if (!self->client_pfd) {
		fprintf (stderr, "Create client pollable fd failed!\n");
		hev_free (self);
		return NULL;
	}

	self->buffer0 = hev_buffer_list_alloc (self->buffer_list);
	if (!self->buffer0) {
		fprintf (stderr, "Alloc buffer failed!\n");
		hev_pollable_fd_destroy (self->client_pfd);
		hev_free (self);
		return NULL;
	}
	left_size = hev_socks5_proto_auth_req_unpack (self->buffer0->data, 0, NULL, NULL);
	self->buffer0->offset = 0;
	self->buffer0->length = 0 - left_size;
	if (!hev_socks5_session_client_read (self, self->buffer0, read_auth_req_handler)) {
		fprintf (stderr, "Read auth request failed!\n");
		hev_buffer_list_free (self->buffer_list, self->buffer0);
		hev_pollable_fd_destroy (self->client_pfd);
		hev_free (self);
		return NULL;
	}

	self->addr.sin_family = AF_INET;

	return self;
}

void
hev_socks5_session_destroy (HevSocks5Session *self)
{
	if (self->socket)
	      hev_socket_destroy (self->socket);
	if (self->resolver)
	      hev_dns_resolver_destroy (self->resolver);
	if (self->buffer0)
	      hev_buffer_list_free (self->buffer_list, self->buffer0);
	if (self->buffer1)
	      hev_buffer_list_free (self->buffer_list, self->buffer1);
	hev_pollable_fd_destroy (self->client_pfd);
	if (self->remote_pfd)
	      hev_pollable_fd_destroy (self->remote_pfd);
	if (-1 < self->remote_fd)
	      close (self->remote_fd);
	close (self->client_fd);
	if (self->notify.notifer)
	      self->notify.notifer (self, self->notify.data);
	hev_free (self);
}

static bool
hev_socks5_session_client_read (HevSocks5Session *self, HevBuffer *buffer,
			HevPollableFDReadyCallback callback)
{
	HevPollableFDReader reader;

	reader.func = sock_reader;
	return hev_pollable_fd_read_async (self->client_pfd, &reader,
				buffer, buffer->length, callback, self);
}

static bool
hev_socks5_session_remote_read (HevSocks5Session *self, HevBuffer *buffer,
			HevPollableFDReadyCallback callback)
{
	HevPollableFDReader reader;

	reader.func = sock_reader;
	return hev_pollable_fd_read_async (self->remote_pfd, &reader,
				buffer, buffer->length, callback, self);
}

static bool
hev_socks5_session_socket_connect (HevSocks5Session *self)
{
	self->socket = hev_socket_new (AF_INET, SOCK_STREAM, 0);
	if (!self->socket)
	      return false;

	if (!hev_socket_connect_async (self->socket, (struct sockaddr *) &self->addr,
					sizeof (self->addr), socket_connect_handler, self))
	      return false;

	return true;
}

static bool
hev_socks5_session_client_write (HevSocks5Session *self, HevBuffer *buffer,
			HevPollableFDReadyCallback callback)
{
	HevPollableFDWriter writer;

	writer.func = sock_writer;
	return hev_pollable_fd_write_async (self->client_pfd, &writer,
				buffer, buffer->length, callback, self);
}

static bool
hev_socks5_session_remote_write (HevSocks5Session *self, HevBuffer *buffer,
			HevPollableFDReadyCallback callback)
{
	HevPollableFDWriter writer;

	writer.func = sock_writer;
	return hev_pollable_fd_write_async (self->remote_pfd, &writer,
				buffer, buffer->length, callback, self);
}

static ssize_t
sock_reader (int fd, void *buf, size_t count, void *user_data)
{
	HevBuffer *buffer = buf;

	return recv (fd, buffer->data + buffer->offset, count, 0);
}

static ssize_t
sock_writer (int fd, void *buf, size_t count, void *user_data)
{
	HevBuffer *buffer = buf;

	return send (fd, buffer->data + buffer->offset, count, 0);
}

void
hev_socks5_session_set_idle (HevSocks5Session *self)
{
	self->is_idle = true;
}

bool
hev_socks5_session_get_idle (HevSocks5Session *self)
{
	return self->is_idle;
}

static void
read_auth_req_handler (HevPollableFD *fd, void *user_data)
{
	HevSocks5Session *self = user_data;
	HevBuffer *buffer;
	ssize_t size;

	self->is_idle = false;
	size = hev_pollable_fd_read_finish (fd, (void **) &buffer);
	if (0 >= size) {
		goto error;
	} else {
		int left_size;
		uint8_t i, method_count, *methods;

		left_size = hev_socks5_proto_auth_req_unpack (buffer->data,
					buffer->offset + size, &method_count, &methods);
		if (0 > left_size) {
			buffer->offset += size;
			buffer->length = 0 - left_size;
			if (!hev_socks5_session_client_read (self, buffer,
							read_auth_req_handler))
			      goto error;
		} else {
			for (i=0; i<method_count; i++) {
				if (HEV_SOCKS5_PROTO_METHOD_NOAUTH == methods[i])
				      break;
			}
			if (i == method_count)
			      goto error;

			buffer->offset = 0;
			buffer->length = hev_socks5_proto_auth_res_pack (buffer->data,
						HEV_SOCKS5_PROTO_METHOD_NOAUTH);
			if (!hev_socks5_session_client_write (self, buffer,
							write_auth_res_handler))
			      goto error;
		}
	}

	return;
error:
	hev_buffer_list_free (self->buffer_list, buffer);
	self->buffer0 = NULL;
	hev_socks5_session_destroy (self);
}

static void
write_auth_res_handler (HevPollableFD *fd, void *user_data)
{
	HevSocks5Session *self = user_data;
	HevBuffer *buffer;
	ssize_t size;

	self->is_idle = false;
	size = hev_pollable_fd_write_finish (fd, (void **) &buffer);
	if (0 >= size) {
		goto error;
	} else {
		if (0 < ((ssize_t) buffer->length - size)) {
			buffer->offset += size;
			buffer->length -= size;
			if (!hev_socks5_session_client_write (self, buffer,
							write_auth_res_handler))
			      goto error;
		} else {
			int left_size;

			left_size = hev_socks5_proto_req_unpack (buffer->data, 0,
						NULL, NULL, NULL, NULL);
			buffer->offset = 0;
			buffer->length = 0 - left_size;
			if (!hev_socks5_session_client_read (self, buffer, read_req_handler))
			      goto error;
		}
	}

	return;
error:
	hev_buffer_list_free (self->buffer_list, buffer);
	self->buffer0 = NULL;
	hev_socks5_session_destroy (self);
}

static void
read_req_handler (HevPollableFD *fd, void *user_data)
{
	HevSocks5Session *self = user_data;
	HevBuffer *buffer;
	ssize_t size;

	self->is_idle = false;
	size = hev_pollable_fd_read_finish (fd, (void **) &buffer);
	if (0 >= size) {
		goto error0;
	} else {
		int left_size;
		uint8_t cmd, atype;
		const char *addr;
		uint16_t port;

		left_size = hev_socks5_proto_req_unpack (buffer->data,
					buffer->offset + size, &cmd, &atype, &addr, &port);
		if (0 > left_size) {
			buffer->offset += size;
			buffer->length = 0 - left_size;
			if (!hev_socks5_session_client_read (self, buffer, read_req_handler))
			      goto error0;
		} else {
			switch (atype) {
			case HEV_SOCKS5_PROTO_ATYPE_IPV4:
				hev_buffer_list_free (self->buffer_list, buffer);
				self->buffer0 = NULL;
				self->addr.sin_port = port;
				self->addr.sin_addr.s_addr = *(uint32_t *) addr;
				if (!hev_socks5_session_socket_connect (self))
				      goto error1;
				break;
			case HEV_SOCKS5_PROTO_ATYPE_DOMAIN:
				hev_buffer_list_free (self->buffer_list, buffer);
				self->buffer0 = NULL;
				self->resolver = hev_dns_resolver_new ("8.8.8.8",
							self->buffer_list);
				if (!self->resolver)
				      goto error1;
				self->addr.sin_port = port;
				if (!hev_dns_resolver_query_async (self->resolver,
								addr, resolver_handler,
								self))
				      goto error1;
				break;
			default:
				buffer->offset = 0;
				buffer->length = hev_socks5_proto_res_pack (buffer->data,
							HEV_SOCKS5_PROTO_REP_ATYPE_NOT_SUPPORT,
							atype, addr, port);
				if (!hev_socks5_session_client_write (self, buffer,
								write_res_handler))
				      goto error0;
				break;
			}
		}
	}

	return;
error0:
	hev_buffer_list_free (self->buffer_list, buffer);
	self->buffer0 = NULL;
error1:
	hev_socks5_session_destroy (self);
}

static void
resolver_handler (HevDNSResolver *resolver, void *user_data)
{
	HevSocks5Session *self = user_data;
	uint32_t ip;

	self->is_idle = false;
	ip = hev_dns_resolver_query_finish (resolver);
	if (0 == ip)
	      goto error;

	self->addr.sin_addr.s_addr = ip;
	if (!hev_socks5_session_socket_connect (self))
	      goto error;

	return;
error:
	hev_socks5_session_destroy (self);
}

static void
socket_connect_handler (HevSocket *socket, void *user_data)
{
	HevSocks5Session *self = user_data;
	const char *addr;

	self->is_idle = false;
	if (0 > hev_socket_connect_finish (socket))
	      goto error;

	self->remote_fd = dup (hev_socket_get_fd (self->socket));
	hev_socket_destroy (self->socket);
	self->socket = NULL;
	self->remote_pfd = hev_pollable_fd_new (self->remote_fd, 1);
	if (!self->remote_pfd)
	      goto error;

	self->buffer0 = hev_buffer_list_alloc (self->buffer_list);
	if (!self->buffer0)
	      goto error;

	self->buffer0->offset = 0;
	addr = (const char *) &self->addr.sin_addr.s_addr;
	self->buffer0->length = hev_socks5_proto_res_pack (self->buffer0->data,
				HEV_SOCKS5_PROTO_REP_SUCC,
				HEV_SOCKS5_PROTO_ATYPE_IPV4,
				addr, self->addr.sin_port);
	if (!hev_socks5_session_client_write (self, self->buffer0, write_res_handler))
	      goto error;

	return;
error:
	hev_socks5_session_destroy (self);
}

static void
write_res_handler (HevPollableFD *fd, void *user_data)
{
	HevSocks5Session *self = user_data;
	HevBuffer *buffer;
	ssize_t size;

	self->is_idle = false;
	size = hev_pollable_fd_write_finish (fd, (void **) &buffer);
	if (0 >= size) {
		goto error1;
	} else {
		if (0 < ((ssize_t) buffer->length - size)) {
			buffer->offset += size;
			buffer->length -= size;
			if (!hev_socks5_session_client_write (self, buffer, write_res_handler))
			      goto error1;
		} else {
			buffer->offset = 0;
			buffer->length = HEV_BUFFER_DATA_SIZE;
			if (!hev_socks5_session_client_read (self, buffer,
							read_client_data_handler))
			      goto error1;

			self->buffer1 = hev_buffer_list_alloc (self->buffer_list);
			if (!self->buffer1)
			      goto error2;
			self->buffer1->offset = 0;
			self->buffer1->length = HEV_BUFFER_DATA_SIZE;
			if (!hev_socks5_session_remote_read (self, self->buffer1,
							read_remote_data_handler))
			      goto error0;
		}
	}

	return;
error0:
	hev_buffer_list_free (self->buffer_list, self->buffer1);
	self->buffer1 = NULL;
error1:
	hev_buffer_list_free (self->buffer_list, buffer);
	self->buffer0 = NULL;
error2:
	hev_socks5_session_destroy (self);
}

static void
read_client_data_handler (HevPollableFD *fd, void *user_data)
{
	HevSocks5Session *self = user_data;
	HevBuffer *buffer;
	ssize_t size;

	self->is_idle = false;
	size = hev_pollable_fd_read_finish (fd, (void **) &buffer);
	if (0 >= size) {
		goto error;
	} else {
		buffer->length = size;
		if (!hev_socks5_session_remote_write (self, buffer,
						write_remote_data_handler))
		      goto error;
	}

	return;
error:
	hev_buffer_list_free (self->buffer_list, buffer);
	self->buffer0 = NULL;
	hev_socks5_session_destroy (self);
}

static void
read_remote_data_handler (HevPollableFD *fd, void *user_data)
{
	HevSocks5Session *self = user_data;
	HevBuffer *buffer;
	ssize_t size;

	self->is_idle = false;
	size = hev_pollable_fd_read_finish (fd, (void **) &buffer);
	if (0 >= size) {
		goto error;
	} else {
		buffer->length = size;
		if (!hev_socks5_session_client_write (self, buffer,
						write_client_data_handler))
		      goto error;
	}

	return;
error:
	hev_buffer_list_free (self->buffer_list, buffer);
	self->buffer1 = NULL;
}

static void
write_client_data_handler (HevPollableFD *fd, void *user_data)
{
	HevSocks5Session *self = user_data;
	HevBuffer *buffer;
	ssize_t size;

	self->is_idle = false;
	size = hev_pollable_fd_write_finish (fd, (void **) &buffer);
	if (0 >= size) {
		goto error;
	} else {
		if (0 < ((ssize_t) buffer->length - size)) {
			buffer->offset += size;
			buffer->length -= size;
			if (!hev_socks5_session_client_write (self, buffer,
							write_client_data_handler))
			      goto error;
		} else {
			buffer->offset = 0;
			buffer->length = HEV_BUFFER_DATA_SIZE;
			if (!hev_socks5_session_remote_read (self, buffer,
							read_remote_data_handler))
			      goto error;
		}
	}

	return;
error:
	hev_buffer_list_free (self->buffer_list, buffer);
	self->buffer1 = NULL;
}

static void
write_remote_data_handler (HevPollableFD *fd, void *user_data)
{
	HevSocks5Session *self = user_data;
	HevBuffer *buffer;
	ssize_t size;

	self->is_idle = false;
	size = hev_pollable_fd_write_finish (fd, (void **) &buffer);
	if (0 >= size) {
		goto error;
	} else {
		if (0 < ((ssize_t) buffer->length - size)) {
			buffer->offset += size;
			buffer->length -= size;
			if (!hev_socks5_session_remote_write (self, buffer,
							write_remote_data_handler))
			      goto error;
		} else {
			buffer->offset = 0;
			buffer->length = HEV_BUFFER_DATA_SIZE;
			if (!hev_socks5_session_client_read (self, buffer,
							read_client_data_handler))
			      goto error;
		}
	}

	return;
error:
	hev_buffer_list_free (self->buffer_list, buffer);
	self->buffer0 = NULL;
	hev_socks5_session_destroy (self);
}

