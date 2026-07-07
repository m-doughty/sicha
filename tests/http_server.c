#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sicha_internal.h"
#include "sicha_thread.h"

#if defined(_WIN32)
/* winsock2.h comes in via sicha_thread.h's windows.h; add the API */
#include <ws2tcpip.h>
#define strncasecmp _strnicmp
typedef SOCKET t_sock;
#define T_BAD_SOCK INVALID_SOCKET
static void t_sock_close(t_sock s)
{
	closesocket(s);
}
static int t_net_init(void)
{
	WSADATA wsa;

	return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}
static void t_net_fini(void)
{
	WSACleanup();
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int t_sock;
#define T_BAD_SOCK (-1)
static void t_sock_close(t_sock s)
{
	close(s);
}
static int t_net_init(void)
{
	return 1;
}
static void t_net_fini(void)
{
}
#endif

/* Readable within ms?  1 yes, 0 timeout, -1 error. */
static int t_sock_wait_readable(t_sock s, int ms)
{
#if defined(_WIN32)
	WSAPOLLFD p;

	p.fd = s;
	p.events = POLLRDNORM;
	p.revents = 0;
	{
		int rc = WSAPoll(&p, 1, ms);

		return rc > 0 ? 1 : rc == 0 ? 0 : -1;
	}
#else
	struct pollfd p;

	p.fd = s;
	p.events = POLLIN;
	p.revents = 0;
	{
		int rc = poll(&p, 1, ms);

		return rc > 0 ? 1 : rc == 0 ? 0 : -1;
	}
#endif
}

static void t_sleep_ms(unsigned ms)
{
	sicha_clock_real.wait_ms(NULL, NULL, ms);
}

#define Q_CAP 16
#define REQ_CAP 32
#define REQ_BYTES_CAP (64 * 1024)

struct t_http_server {
	t_sock listen_fd;
	uint16_t port;
	sicha_thread thread;
	sicha_mutex mu;
	int stop;                   /* guarded by mu                    */
	t_http_response queue[Q_CAP];
	char *queue_bodies[Q_CAP];  /* owned copies                     */
	char *queue_extras[Q_CAP];
	int q_len;
	int q_head;
	char *reqs[REQ_CAP];
	int conn_ids[REQ_CAP];
	int n_reqs;
	int next_conn_id;
};

static int server_should_stop(t_http_server *s)
{
	int v;

	sicha_mutex_lock(&s->mu);
	v = s->stop;
	sicha_mutex_unlock(&s->mu);
	return v;
}

/* Pop the next scripted response; 0 if the queue is empty. */
static int server_pop(t_http_server *s, t_http_response *out,
	char **body_out, char **extra_out)
{
	int ok = 0;

	sicha_mutex_lock(&s->mu);
	if (s->q_head < s->q_len) {
		*out = s->queue[s->q_head];
		*body_out = s->queue_bodies[s->q_head];
		*extra_out = s->queue_extras[s->q_head];
		s->q_head++;
		ok = 1;
	}
	sicha_mutex_unlock(&s->mu);
	return ok;
}

static void server_record(t_http_server *s, const char *req, size_t len,
	int conn_id)
{
	sicha_mutex_lock(&s->mu);
	if (s->n_reqs < REQ_CAP) {
		char *copy = malloc(len + 1);

		if (copy != NULL) {
			memcpy(copy, req, len);
			copy[len] = '\0';
			s->reqs[s->n_reqs] = copy;
			s->conn_ids[s->n_reqs] = conn_id;
			s->n_reqs++;
		}
	}
	sicha_mutex_unlock(&s->mu);
}

/* Read one full request (headers + Content-Length body).  Returns the
 * total bytes, 0 on clean close/timeout, -1 on error. */
static long read_request(t_sock fd, char *buf, size_t cap)
{
	size_t got = 0;
	size_t need = 0;
	const char *hdr_end = NULL;

	for (;;) {
		long n;
		int r = t_sock_wait_readable(fd, 2000);

		if (r <= 0) {
			return 0; /* peer idle/gone */
		}
		n = (long)recv(fd, buf + got, (int)(cap - got - 1), 0);
		if (n <= 0) {
			return 0;
		}
		got += (size_t)n;
		buf[got] = '\0';
		if (hdr_end == NULL) {
			hdr_end = strstr(buf, "\r\n\r\n");
			if (hdr_end != NULL) {
				const char *cl = NULL;
				const char *p = buf;

				/* case-insensitive content-length scan */
				while ((p = strchr(p, '\n')) != NULL) {
					p++;
					if (strncasecmp(p,
						"content-length:", 15) ==
						0) {
						cl = p + 15;
						break;
					}
				}
				need = (size_t)(hdr_end + 4 - buf);
				if (cl != NULL) {
					need += (size_t)strtoul(cl, NULL,
						10);
				}
			}
		}
		if (hdr_end != NULL && got >= need) {
			return (long)got;
		}
		if (got >= cap - 1) {
			return -1;
		}
	}
}

static void send_all(t_sock fd, const char *bytes, size_t len)
{
	size_t off = 0;

	while (off < len) {
		long n = (long)send(fd, bytes + off,
			(int)(len - off), 0);

		if (n <= 0) {
			return;
		}
		off += (size_t)n;
	}
}

/* Serve requests on one connection until close/stop; returns when the
 * connection is done. */
static void serve_connection(t_http_server *s, t_sock fd, int conn_id)
{
	char *req_buf = malloc(REQ_BYTES_CAP);

	if (req_buf == NULL) {
		return;
	}
	for (;;) {
		long got;
		t_http_response resp;
		char *body = NULL;
		char *extra = NULL;

		if (server_should_stop(s)) {
			break;
		}
		got = read_request(fd, req_buf, REQ_BYTES_CAP);
		if (got <= 0) {
			break;
		}
		server_record(s, req_buf, (size_t)got, conn_id);
		if (!server_pop(s, &resp, &body, &extra)) {
			static const char nothing[] =
				"HTTP/1.1 500 Internal Server Error\r\n"
				"Content-Length: 0\r\n\r\n";

			send_all(fd, nothing, sizeof(nothing) - 1);
			break;
		}
		if (resp.pre_status_delay_ms > 0) {
			t_sleep_ms(resp.pre_status_delay_ms);
		}
		if (server_should_stop(s)) {
			break;
		}
		{
			char head[1024];
			size_t body_len = body != NULL ?
				resp.body_len : 0;
			size_t advertised = resp.advertised_len != 0 ?
				resp.advertised_len : body_len;
			int n = snprintf(head, sizeof(head),
				"HTTP/1.1 %d X\r\n"
				"Content-Length: %zu\r\n"
				"%s"
				"%s"
				"\r\n",
				resp.status, advertised,
				resp.close_connection ?
					"Connection: close\r\n" : "",
				extra != NULL ? extra : "");

			send_all(fd, head, (size_t)n);
		}
		{
			size_t off = 0;

			while (body != NULL && off < resp.body_len) {
				size_t n = resp.chunk_size == 0 ?
					resp.body_len - off :
					resp.chunk_size;

				if (n > resp.body_len - off) {
					n = resp.body_len - off;
				}
				if (resp.per_chunk_delay_ms > 0) {
					t_sleep_ms(
						resp.per_chunk_delay_ms);
				}
				if (server_should_stop(s)) {
					break;
				}
				send_all(fd, body + off, n);
				off += n;
			}
		}
		if (resp.close_connection ||
			(resp.advertised_len != 0 &&
				resp.advertised_len > resp.body_len)) {
			break; /* premature close: mid-body loss */
		}
	}
	free(req_buf);
}

static void server_main(void *arg)
{
	t_http_server *s = arg;

	while (!server_should_stop(s)) {
		int r = t_sock_wait_readable(s->listen_fd, 50);

		if (r > 0) {
			t_sock fd = accept(s->listen_fd, NULL, NULL);
			int conn_id;

			if (fd == T_BAD_SOCK) {
				continue;
			}
			sicha_mutex_lock(&s->mu);
			conn_id = s->next_conn_id++;
			sicha_mutex_unlock(&s->mu);
			serve_connection(s, fd, conn_id);
			t_sock_close(fd);
		}
	}
}

t_http_server *t_http_server_start(void)
{
	t_http_server *s;
	struct sockaddr_in addr;
	socklen_t alen = sizeof(addr);

	if (!t_net_init()) {
		return NULL;
	}
	s = calloc(1, sizeof(*s));
	if (s == NULL) {
		t_net_fini();
		return NULL;
	}
	sicha_mutex_init(&s->mu);
	s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (s->listen_fd == T_BAD_SOCK) {
		free(s);
		t_net_fini();
		return NULL;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(s->listen_fd, (struct sockaddr *)&addr,
			sizeof(addr)) != 0 ||
		listen(s->listen_fd, 16) != 0 ||
		getsockname(s->listen_fd, (struct sockaddr *)&addr,
			&alen) != 0) {
		t_sock_close(s->listen_fd);
		free(s);
		t_net_fini();
		return NULL;
	}
	s->port = ntohs(addr.sin_port);
	if (!sicha_thread_create(&s->thread, server_main, s)) {
		t_sock_close(s->listen_fd);
		free(s);
		t_net_fini();
		return NULL;
	}
	return s;
}

uint16_t t_http_server_port(const t_http_server *s)
{
	return s->port;
}

void t_http_server_push(t_http_server *s, const t_http_response *r)
{
	sicha_mutex_lock(&s->mu);
	if (s->q_len < Q_CAP) {
		t_http_response copy = *r;
		size_t blen = 0;

		if (copy.body != NULL) {
			blen = copy.body_len == (size_t)-1 ?
				strlen(copy.body) : copy.body_len;
			s->queue_bodies[s->q_len] = malloc(blen + 1);
			if (s->queue_bodies[s->q_len] != NULL) {
				memcpy(s->queue_bodies[s->q_len],
					copy.body, blen);
				s->queue_bodies[s->q_len][blen] = '\0';
			}
		} else {
			s->queue_bodies[s->q_len] = NULL;
		}
		copy.body_len = blen;
		if (copy.extra_headers != NULL) {
			size_t elen = strlen(copy.extra_headers);

			s->queue_extras[s->q_len] = malloc(elen + 1);
			if (s->queue_extras[s->q_len] != NULL) {
				memcpy(s->queue_extras[s->q_len],
					copy.extra_headers, elen + 1);
			}
		} else {
			s->queue_extras[s->q_len] = NULL;
		}
		s->queue[s->q_len] = copy;
		s->q_len++;
	}
	sicha_mutex_unlock(&s->mu);
}

int t_http_server_request_count(t_http_server *s)
{
	int n;

	sicha_mutex_lock(&s->mu);
	n = s->n_reqs;
	sicha_mutex_unlock(&s->mu);
	return n;
}

const char *t_http_server_request(t_http_server *s, int i,
	int *conn_id_out)
{
	const char *req = NULL;

	sicha_mutex_lock(&s->mu);
	if (i < s->n_reqs) {
		req = s->reqs[i];
		if (conn_id_out != NULL) {
			*conn_id_out = s->conn_ids[i];
		}
	}
	sicha_mutex_unlock(&s->mu);
	return req;
}

void t_http_server_stop(t_http_server *s)
{
	if (s == NULL) {
		return;
	}
	sicha_mutex_lock(&s->mu);
	s->stop = 1;
	sicha_mutex_unlock(&s->mu);
	sicha_thread_join(s->thread);
	t_sock_close(s->listen_fd);
	for (int i = 0; i < s->n_reqs; i++) {
		free(s->reqs[i]);
	}
	for (int i = 0; i < s->q_len; i++) {
		free(s->queue_bodies[i]);
		free(s->queue_extras[i]);
	}
	sicha_mutex_destroy(&s->mu);
	free(s);
	t_net_fini();
}

uint16_t t_free_port(void)
{
	t_sock fd;
	struct sockaddr_in addr;
	socklen_t alen = sizeof(addr);
	uint16_t port = 0;

	if (!t_net_init()) {
		return 0;
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == T_BAD_SOCK) {
		t_net_fini();
		return 0;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
		getsockname(fd, (struct sockaddr *)&addr, &alen) == 0) {
		port = ntohs(addr.sin_port);
	}
	t_sock_close(fd);
	t_net_fini();
	return port;
}
