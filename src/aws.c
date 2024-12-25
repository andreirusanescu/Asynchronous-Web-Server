// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

/**
 * Maybe add a function that returns the type of a file based on filename
 * for header(Content-Type: ...) and more info about the file (mtime) or
 * date
 */
static void connection_prepare_send_reply_header(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the reply header. */
	const char *header =
		"HTTP/1.1 200 OK\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n"
		"\r\n";

	sprintf(conn->send_buffer, header, conn->file_size);
	conn->send_len = strlen(conn->send_buffer);
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the 404 header. */
	const char *header =
		"HTTP/1.1 404 Not Found\r\n"
		"Content-Length: 0\r\n"
		"Connection: close\r\n"
		"\r\n";

	strcpy(conn->send_buffer, header);
	conn->send_len = strlen(conn->send_buffer);
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* TODO: Get resource type depending on request path/filename. Filename should
	 * point to the static or dynamic folder.
	 */
	if (strstr(conn->filename, "static")) {
		conn->res_type = RESOURCE_TYPE_STATIC;
		return RESOURCE_TYPE_STATIC;
	}

	if (strstr(conn->filename, "dynamic")) {
		conn->res_type = RESOURCE_TYPE_DYNAMIC;
		return RESOURCE_TYPE_DYNAMIC;
	}

	return RESOURCE_TYPE_NONE;
}

struct connection *connection_create(int sockfd)
{
	/* TODO: Initialize connection structure on given socket. */
	struct connection *res = (struct connection *)calloc(1, sizeof(struct connection));

	DIE(!res, "calloc() failed");
	res->sockfd = sockfd;
	res->fd = -1;

	return res;
}

void connection_start_async_io(struct connection *conn)
{
	/* TODO: Start asynchronous operation (read from file).
	 * Use io_submit(2) & friends for reading data asynchronously.
	 */
	struct io_event events;
	ssize_t rc;

	/**
	 * Before this call, the buffer was already zeroed
	 * and was filled when the header was sent
	 */
	memset(conn->send_buffer, 0, conn->send_len);
	conn->send_len = conn->send_pos = 0;

	while (conn->async_read_len < conn->file_size) {
		/* Prepares for reading from the file */
		io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer,
					  sizeof(conn->send_buffer), conn->async_read_len);

		/**
		 * Submits the operation of read / write to the
		 * kernel to process it in the background
		 */
		rc = io_submit(conn->ctx, 1, &conn->piocb[0]);
		DIE(rc < 0, "io_submit() failed");

		/* Kernel puts how much it read in the event
		 * structure (event.res), this call asks the kernel
		 * if the operation is done. Timeout is set to
		 * NULL so it will block indefinitely until
		 * the read event has been obtained from the internal
		 * queue
		 */
		rc = io_getevents(conn->ctx, 1, 1, &events, NULL);
		DIE(rc != 1, "io_getevents() failed");

		conn->async_read_len += events.res;

		io_prep_pwrite(&conn->iocb, conn->sockfd, conn->send_buffer,
					   events.res, 0);

		rc = io_submit(conn->ctx, 1, &conn->piocb[0]);
		DIE(rc < 0, "io_submit() failed");

		rc = io_getevents(conn->ctx, 1, 1, &events, NULL);
		DIE(rc != 1, "io_getevents() failed");

		memset(conn->send_buffer, 0, events.res);
	}

	dlog(LOG_DEBUG, "Sent %ld bytes out of %ld\n", conn->async_read_len, conn->file_size);
}

void connection_remove(struct connection *conn)
{
	/* TODO: Remove connection handler. */
	int rc;

	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr() failed");

	rc = close(conn->sockfd);
	DIE(rc < 0, "close() failed");

	/**
	 * Check if the fd was set
	 */
	if (conn->fd > 0) {
		rc = close(conn->fd);
		DIE(rc < 0, "close() failed");
	}

	dlog(LOG_DEBUG, "Closed the connection\n");
	free(conn);
}

void handle_new_connection(void)
{
	/* TODO: Handle a new connection request on the server socket. */
	int sockfd, rc;
	struct sockaddr_in client_address;
	socklen_t client_length = sizeof(struct sockaddr_in);
	struct connection *connection;

	/* TODO: Accept new connection. */
	sockfd = accept(listenfd, (SSA *) &client_address, &client_length);
	DIE(sockfd < 0, "accept() failed");
	dlog(LOG_INFO, "Accepted connection from %s:%d\n",
		inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port));

	/* TODO: Set socket to be non-blocking. */
	rc = fcntl(sockfd, F_GETFL);
	DIE(rc < 0, "fcntl() failed");

	rc = fcntl(sockfd, F_SETFL, O_NONBLOCK | rc);
	DIE(rc < 0, "fcntl() failed");

	/* TODO: Instantiate new connection handler. */
	connection = connection_create(sockfd);

	/* TODO: Add socket to epoll. */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, connection);
	DIE(rc == -1, "w_epoll_add_ptr_in() failed");

	/* TODO: Initialize HTTP_REQUEST parser. */
	/* Each connection has its own parser */
	http_parser_init(&connection->request_parser, HTTP_REQUEST);

	/* sets callback to connection, used in aws_on_path_cb */
	connection->request_parser.data = connection;
}

/* Receive message on socket.
 * Store message in recv_buffer in struct connection.
 */
void receive_data(struct connection *conn)
{
	ssize_t bytes = 0;
	int rc;
	char address_buffer[64];

	dlog(LOG_DEBUG, "Receiving data...\n");
	conn->state = STATE_RECEIVING_DATA;

	/* Gets the adress and port in address_buffer */
	rc = get_peer_address(conn->sockfd, address_buffer, sizeof(address_buffer));
	if (rc < 0) {
		ERR("get_perr_address() failed");
		conn->state = STATE_CONNECTION_CLOSED;
		return;
	}

	while (1) {
		bytes = recv(conn->sockfd, conn->recv_buffer + conn->recv_len,
					 sizeof(conn->recv_buffer) - conn->recv_len, 0);
		if (bytes > 0)
			conn->recv_len += bytes;
		else
			break;
	}

	if (!conn->recv_len) {
		conn->state = STATE_CONNECTION_CLOSED;
		return;
	}

	dlog(LOG_DEBUG, "Received message from: %s\n", address_buffer);

	printf("--\n%s--\n", conn->recv_buffer);
}

int connection_open_file(struct connection *conn)
{
	/* TODO: Open file and update connection fields. */
	int rc;
	struct stat stats;

	conn->fd = open(conn->filename, O_RDONLY);
	if (conn->fd == -1) {
		dlog(LOG_DEBUG, "open() failed\n");
		connection_prepare_send_404(conn);
		conn->state = STATE_SENDING_404;
		return -1;
	}

	rc = fstat(conn->fd, &stats);
	DIE(rc < 0, "fstat() failed");
	conn->file_size = stats.st_size;

	connection_prepare_send_reply_header(conn);
	conn->state = STATE_SENDING_HEADER;

	return 0;
}

void connection_complete_async_io(struct connection *conn)
{
	/* TODO: Complete asynchronous operation; operation returns successfully.
	 * Prepare socket for sending.
	 */
}

int parse_header(struct connection *conn)
{
	/* TODO: Parse the HTTP header and extract the file path. */
	/* Use mostly null settings except for on_path callback. */
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};

	dlog(LOG_DEBUG, "Parsing header\n");

	http_parser_execute(&conn->request_parser, &settings_on_path, conn->recv_buffer,
											  conn->recv_len);

	if (conn->have_path == 0) {
		dlog(LOG_DEBUG, "Bad header\n");
		conn->state = STATE_SENDING_404;
		return -1;
	}

	/* request_path starts with '/' */
	strcpy(conn->filename, conn->request_path + 1);
	conn->state = STATE_REQUEST_RECEIVED;

	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	/* TODO: Send static data using sendfile(2). */
	ssize_t bytes;
	size_t total = 0;

	while (1) {
		bytes = sendfile(conn->sockfd, conn->fd, NULL, BUFSIZ);
		if (bytes == 0)
			break;
		if (bytes > 0)
			total += bytes;
	}
	conn->state = STATE_DATA_SENT;
	dlog(LOG_DEBUG, "Sent %ld bytes out of %ld\n", total, conn->file_size);

	return 0;
}

/**
 * Sends the header
 */
int connection_send_data(struct connection *conn)
{
	/* May be used as a helper function. */
	/* TODO: Send as much data as possible from the connection send buffer.
	 * Returns the number of bytes sent or -1 if an error occurred
	 */
	ssize_t bytes = 0;

	while (1) {
		bytes = send(conn->sockfd, conn->send_buffer + conn->send_pos,
					 conn->send_len - conn->send_pos, 0);
		if (bytes == 0)
			break;
		if (bytes > 0)
			conn->send_pos += bytes;
	}

	if (!conn->send_pos) {
		connection_remove(conn);
		return 0;
	}

	dlog(LOG_DEBUG, "Sent %ld bytes out of %ld of header\n", conn->send_pos, conn->send_len);

	return conn->send_pos;
}

int connection_send_dynamic(struct connection *conn)
{
	/* TODO: Read data asynchronously.
	 * Returns 0 on success and -1 on error.
	 */
	int rc;

	/* Initialize context to 0 prior to io_setup() */
	conn->ctx = 0;

	/**
	 * Internal queue of context is of length 1
	 * due to the fact that the internal piocb vector
	 * is of size 1
	 */
	rc = io_setup(1, &conn->ctx);
	DIE(rc < 0, "io_setup() failed");

	conn->piocb[0] = &conn->iocb;

	connection_start_async_io(conn);

	conn->state = STATE_DATA_SENT;

	rc = io_destroy(conn->ctx);
	DIE(rc < 0, "io_destroy() failed");

	return 0;
}

void handle_input(struct connection *conn)
{
	/* TODO: Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */
	int rc;

	while (1) {
		switch (conn->state) {
		case STATE_INITIAL:
			receive_data(conn);
			break;
		case STATE_RECEIVING_DATA:
			parse_header(conn);
			break;
		case STATE_REQUEST_RECEIVED:
			connection_open_file(conn);
			break;
		case STATE_CONNECTION_CLOSED:
			connection_remove(conn);
			return;
		default:
			dlog(LOG_DEBUG, "Waiting for output activity...\n");
			rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
			DIE(rc == -1, "w_epoll_mod_ptr_out() failed");
			return;
		}
	}
}

void handle_output(struct connection *conn)
{
	/* TODO: Handle output information: may be a new valid requests or notification of
	 * completion of an asynchronous I/O operation or invalid requests.
	 */
	while (1) {
		switch (conn->state) {
		case STATE_SENDING_404:
			connection_send_data(conn);
			conn->state = STATE_404_SENT;
			break;
		case STATE_SENDING_HEADER:
			connection_send_data(conn);
			conn->state = STATE_HEADER_SENT;

			switch (connection_get_resource_type(conn)) {
			case RESOURCE_TYPE_STATIC:
				connection_send_static(conn);
				break;
			case RESOURCE_TYPE_DYNAMIC:
				connection_send_dynamic(conn);
				break;
			default:
				ERR("RESOURCE TYPE NONE\n");
				break;
			}

		case STATE_404_SENT:
		case STATE_DATA_SENT:
		case STATE_HEADER_SENT:
			connection_remove(conn);
			return;
		default:
			ERR("Unexpected state\n");
			exit(1);
		}
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	/* TODO: Handle new client. There can be input and output connections.
	 * Take care of what happened at the end of a connection.
	 */

	if (event & EPOLLIN) {
		dlog(LOG_DEBUG, "New message\n");
		handle_input(conn);
	} else if (event & EPOLLOUT) {
		dlog(LOG_DEBUG, "Sending message\n");
		handle_output(conn);
	}
}

int main(void)
{
	int rc;

	/* TODO: Initialize asynchronous operations. */
	rc = io_setup(1, &ctx);
	DIE(rc != 0, "io_setup() failed");

	/* TODO: Initialize multiplexing. */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create() failed");

	/* TODO: Create server socket. */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);

	/* TODO: Add server socket to epoll object*/
	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc == -1, "w_epoll_add_fd_in() failed");

	/* Uncomment the following line for debugging. */
	dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* TODO: Wait for events. */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/* TODO: Switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */
		if (rev.data.fd == listenfd) {
			dlog(LOG_DEBUG, "New connection\n");
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			handle_client(rev.events, rev.data.ptr);
		}
	}

	rc = io_destroy(ctx);
	DIE(rc == -1, "io_destroy() failed");

	rc = close(listenfd);
	DIE(rc == -1, "close() failed");

	rc = close(epollfd);
	DIE(rc == -1, "close() failed");

	return 0;
}
