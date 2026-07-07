/* A scriptable loopback HTTP/1.1 server for integration-testing the
 * curl transport over real sockets.  Plain HTTP only (TLS belongs to
 * libcurl).  Responses are scripted FIFO; every request is recorded
 * verbatim together with the id of the connection that carried it
 * (so connection REUSE is assertable).  Keep-alive by default;
 * scripted delays and premature closes simulate slow headers, stalled
 * bodies, and mid-body resets. */

#ifndef SICHA_TEST_HTTP_SERVER_H
#define SICHA_TEST_HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef struct t_http_response {
	int status;                 /* e.g. 200                         */
	const char *extra_headers;  /* raw "K: v\r\n..." or NULL        */
	const char *body;           /* NULL = empty                     */
	size_t body_len;            /* (size_t)-1 = strlen              */
	size_t chunk_size;          /* split body writes; 0 = one write */
	unsigned pre_status_delay_ms;
	unsigned per_chunk_delay_ms;
	/* advertise MORE bytes than sent, then close: the client sees a
	 * mid-body connection loss.  0 = advertise the real length. */
	size_t advertised_len;
	int close_connection;       /* force close after this response  */
} t_http_response;

typedef struct t_http_server t_http_server;

t_http_server *t_http_server_start(void);   /* 127.0.0.1, ephemeral   */
uint16_t t_http_server_port(const t_http_server *s);
void t_http_server_push(t_http_server *s, const t_http_response *r);
int t_http_server_request_count(t_http_server *s);
/* Raw recorded request bytes (headers + body), NUL-terminated; NULL
 * if out of range.  conn_id_out (optional) receives the serving
 * connection's id. */
const char *t_http_server_request(t_http_server *s, int i,
	int *conn_id_out);
void t_http_server_stop(t_http_server *s);

/* An ephemeral port with NOTHING listening (for refused-connection
 * tests); 0 on failure. */
uint16_t t_free_port(void);

#endif /* SICHA_TEST_HTTP_SERVER_H */
