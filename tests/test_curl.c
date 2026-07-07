/* Integration: the full client running over the built-in libcurl
 * transport against a real loopback HTTP/1.1 server.  Covers the
 * happy paths (blocking + streaming), wire assertions (HTTP/1.1
 * request line, auth header, POST path), slow-header and stalled-body
 * timeouts, mid-body loss + retry, Retry-After over the wire,
 * connection-refused fallback, connection reuse across sequential
 * requests, and cross-thread cancellation latency.
 *
 * Real-clock timing assertions are LOOSE (upper bounds in seconds)
 * so noisy CI boxes don't flake. */

#include <stdlib.h>

#include "engine_helpers.h"
#include "http_server.h"
#include "sicha_thread.h"

/* Build a real client against the loopback server (curl transport =
 * NULL transport, real clock). */
static sicha_client *mk_client(uint16_t port,
	const sicha_retry_policy *policy, const char *api_key)
{
	char url[64];
	sicha_backend_desc d;
	sicha_client_opts opts;
	sicha_client *client = NULL;

	snprintf(url, sizeof(url), "http://127.0.0.1:%u/v1",
		(unsigned)port);
	d = t_backend(url, "it-model");
	d.api_key = api_key;
	/* tight budgets so failure tests stay fast */
	d.timeouts.connect_ms = 2000;
	d.timeouts.first_byte_ms = 2000;
	d.timeouts.idle_ms = 2000;
	d.timeouts.total_ms = 10000;
	memset(&opts, 0, sizeof(opts));
	opts.struct_size = (uint32_t)sizeof(opts);
	opts.backends = &d;
	opts.n_backends = 1;
	if (policy != NULL) {
		opts.retry = *policy;
	}
	opts.prng_seed = 42;
	T_CHECK(sicha_client_create(&opts, &client) == SICHA_OK);
	return client;
}

static void check_blocking_happy(void)
{
	t_http_server *srv = t_http_server_start();
	sicha_client *client;
	sicha_result *r = NULL;
	t_http_response resp;

	T_CHECK(srv != NULL);
	client = mk_client(t_http_server_port(srv), NULL, "sk-int-test");
	memset(&resp, 0, sizeof(resp));
	resp.status = 200;
	resp.body = T_OK_BODY("over the wire");
	resp.body_len = (size_t)-1;
	t_http_server_push(srv, &resp);
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(strcmp(sicha_result_text(r, NULL), "over the wire") == 0);
	T_CHECK(sicha_result_prompt_tokens(r) == 5);
	/* wire assertions from the recorded raw request */
	T_CHECK(t_http_server_request_count(srv) == 1);
	{
		const char *raw = t_http_server_request(srv, 0, NULL);

		T_CHECK(raw != NULL);
		T_CHECK(strncmp(raw, "POST /v1/chat/completions HTTP/1.1"
			"\r\n", 36) == 0);
		T_CHECK(strstr(raw, "Authorization: Bearer sk-int-test")
			!= NULL);
		T_CHECK(strstr(raw, "Content-Type: application/json")
			!= NULL);
		T_CHECK(strstr(raw, "User-Agent: sicha/") != NULL);
		T_CHECK(strstr(raw, "\"model\":\"it-model\"") != NULL);
		T_CHECK(strstr(raw, "\"content\":\"hi\"") != NULL);
	}
	sicha_result_destroy(r);
	sicha_client_destroy(client);
	t_http_server_stop(srv);
}

typedef struct it_deltas {
	char text[256];
	size_t len;
	int calls;
} it_deltas;

static int32_t it_on_delta(void *ud, const char *bytes, size_t len)
{
	it_deltas *d = ud;

	memcpy(d->text + d->len, bytes, len);
	d->len += len;
	d->calls++;
	return 0;
}

static void check_streaming_happy(void)
{
	t_http_server *srv = t_http_server_start();
	sicha_client *client;
	sicha_result *r = NULL;
	t_http_response resp;
	it_deltas deltas;
	sicha_callbacks cbs;

	T_CHECK(srv != NULL);
	client = mk_client(t_http_server_port(srv), NULL, NULL);
	memset(&resp, 0, sizeof(resp));
	resp.status = 200;
	resp.extra_headers = "Content-Type: text/event-stream\r\n";
	resp.body = T_SSE_BODY("str", "eamed");
	resp.body_len = (size_t)-1;
	resp.chunk_size = 7;          /* force many tiny TCP writes */
	resp.per_chunk_delay_ms = 5;
	t_http_server_push(srv, &resp);

	memset(&deltas, 0, sizeof(deltas));
	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = &deltas;
	cbs.on_delta = it_on_delta;
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat_stream(client, &req, &cbs, NULL,
			&r) == SICHA_OK);
	}
	T_CHECK(deltas.len == 8 &&
		memcmp(deltas.text, "streamed", 8) == 0);
	T_CHECK(strcmp(sicha_result_text(r, NULL), "streamed") == 0);
	T_CHECK(sicha_result_finish_reason(r) == SICHA_FINISH_STOP);
	/* streamed via Accept header */
	{
		const char *raw = t_http_server_request(srv, 0, NULL);

		T_CHECK(strstr(raw, "Accept: text/event-stream") != NULL);
		T_CHECK(strstr(raw, "\"stream\":true") != NULL);
	}
	sicha_result_destroy(r);
	sicha_client_destroy(client);
	t_http_server_stop(srv);
}

static void check_slow_headers_timeout(void)
{
	t_http_server *srv = t_http_server_start();
	sicha_retry_policy pol;
	sicha_client *client;
	sicha_result *r = NULL;
	t_http_response resp;
	char url[64];
	sicha_backend_desc d;
	sicha_client_opts opts;

	T_CHECK(srv != NULL);
	/* dedicated client: first_byte budget 300 ms */
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/v1",
		(unsigned)t_http_server_port(srv));
	d = t_backend(url, "m");
	d.timeouts.first_byte_ms = 300;
	memset(&pol, 0, sizeof(pol));
	pol.max_tries = 1; /* fail fast to exhaustion */
	memset(&opts, 0, sizeof(opts));
	opts.struct_size = (uint32_t)sizeof(opts);
	opts.backends = &d;
	opts.n_backends = 1;
	opts.retry = pol;
	opts.prng_seed = 42;
	T_CHECK(sicha_client_create(&opts, &client) == SICHA_OK);

	memset(&resp, 0, sizeof(resp));
	resp.status = 200;
	resp.body = T_OK_BODY("late");
	resp.body_len = (size_t)-1;
	resp.pre_status_delay_ms = 3000; /* way past the budget */
	t_http_server_push(srv, &resp);
	{
		sicha_request req = t_req1();
		uint64_t t0 = sicha_clock_real.now_ms(NULL);

		T_CHECK(sicha_chat(client, &req, NULL, NULL, &r) ==
			SICHA_E_EXHAUSTED);
		/* fired around 300 ms, certainly under 2.5 s */
		T_CHECK(sicha_clock_real.now_ms(NULL) - t0 < 2500);
	}
	T_CHECK(sicha_result_attempt_count(r) == 1);
	T_CHECK(sicha_result_attempt(r, 0)->transport_status ==
		SICHA_T_E_TIMEOUT_FIRST_BYTE);
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_ADVANCE);
	sicha_result_destroy(r);
	sicha_client_destroy(client);
	t_http_server_stop(srv);
}

static void check_stalled_body_idle_timeout(void)
{
	t_http_server *srv = t_http_server_start();
	sicha_retry_policy pol;
	sicha_client *client;
	sicha_result *r = NULL;
	t_http_response resp;
	char url[64];
	sicha_backend_desc d;
	sicha_client_opts opts;

	T_CHECK(srv != NULL);
	snprintf(url, sizeof(url), "http://127.0.0.1:%u/v1",
		(unsigned)t_http_server_port(srv));
	d = t_backend(url, "m");
	d.timeouts.idle_ms = 300;
	memset(&pol, 0, sizeof(pol));
	pol.max_tries = 1;
	memset(&opts, 0, sizeof(opts));
	opts.struct_size = (uint32_t)sizeof(opts);
	opts.backends = &d;
	opts.n_backends = 1;
	opts.retry = pol;
	opts.prng_seed = 42;
	T_CHECK(sicha_client_create(&opts, &client) == SICHA_OK);

	memset(&resp, 0, sizeof(resp));
	resp.status = 200;
	resp.body = T_OK_BODY("stalls");
	resp.body_len = (size_t)-1;
	resp.chunk_size = 4;
	resp.per_chunk_delay_ms = 3000; /* stalls between chunks */
	t_http_server_push(srv, &resp);
	{
		sicha_request req = t_req1();
		uint64_t t0 = sicha_clock_real.now_ms(NULL);

		T_CHECK(sicha_chat(client, &req, NULL, NULL, &r) ==
			SICHA_E_EXHAUSTED);
		T_CHECK(sicha_clock_real.now_ms(NULL) - t0 < 5000);
	}
	T_CHECK(sicha_result_attempt(r, 0)->transport_status ==
		SICHA_T_E_TIMEOUT_IDLE);
	sicha_result_destroy(r);
	sicha_client_destroy(client);
	t_http_server_stop(srv);
}

static void check_mid_body_loss_retries(void)
{
	/* content-length larger than what arrives: curl reports a
	 * reset-class failure; no callbacks, so the engine retries and
	 * the second attempt lands */
	t_http_server *srv = t_http_server_start();
	sicha_retry_policy pol;
	sicha_client *client;
	sicha_result *r = NULL;
	t_http_response broken;
	t_http_response good;

	T_CHECK(srv != NULL);
	memset(&pol, 0, sizeof(pol));
	pol.backoff_base_ms = 10;
	pol.backoff_cap_ms = 50;
	pol.backoff_jitter_ms = SICHA_DISABLED;
	client = mk_client(t_http_server_port(srv), &pol, NULL);

	memset(&broken, 0, sizeof(broken));
	broken.status = 200;
	broken.body = T_OK_BODY("cut short");
	broken.body_len = 20; /* deliver a prefix... */
	broken.advertised_len = 4096; /* ...of a bigger promise */
	t_http_server_push(srv, &broken);
	memset(&good, 0, sizeof(good));
	good.status = 200;
	good.body = T_OK_BODY("second attempt");
	good.body_len = (size_t)-1;
	t_http_server_push(srv, &good);
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(strcmp(sicha_result_text(r, NULL),
		"second attempt") == 0);
	T_CHECK(sicha_result_attempt_count(r) == 2);
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_RETRY_SAME);
	T_CHECK(sicha_result_attempt(r, 0)->transport_status ==
		SICHA_T_E_RESET);
	sicha_result_destroy(r);
	sicha_client_destroy(client);
	t_http_server_stop(srv);
}

static void check_retry_after_over_the_wire(void)
{
	t_http_server *srv = t_http_server_start();
	sicha_retry_policy pol;
	sicha_status_override ov = { 429, SICHA_CLASS_RETRY_SAME };
	sicha_client *client;
	sicha_result *r = NULL;
	t_http_response limited;
	t_http_response good;

	T_CHECK(srv != NULL);
	memset(&pol, 0, sizeof(pol));
	pol.overrides = &ov;
	pol.n_overrides = 1;
	client = mk_client(t_http_server_port(srv), &pol, NULL);

	memset(&limited, 0, sizeof(limited));
	limited.status = 429;
	limited.extra_headers = "Retry-After: 0\r\n";
	limited.body = "{\"error\":\"slow down\"}";
	limited.body_len = (size_t)-1;
	t_http_server_push(srv, &limited);
	memset(&good, 0, sizeof(good));
	good.status = 200;
	good.body = T_OK_BODY("after the wait");
	good.body_len = (size_t)-1;
	t_http_server_push(srv, &good);
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(strcmp(sicha_result_text(r, NULL),
		"after the wait") == 0);
	T_CHECK(sicha_result_attempt(r, 0)->http_status == 429);
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_RETRY_SAME);
	sicha_result_destroy(r);
	sicha_client_destroy(client);
	t_http_server_stop(srv);
}

static void check_connection_refused(void)
{
	uint16_t port = t_free_port();
	sicha_retry_policy pol;
	sicha_client *client;
	sicha_result *r = NULL;

	T_CHECK(port != 0);
	memset(&pol, 0, sizeof(pol));
	pol.max_tries = 1;
	client = mk_client(port, &pol, NULL);
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(client, &req, NULL, NULL, &r) ==
			SICHA_E_EXHAUSTED);
	}
	T_CHECK(sicha_result_attempt_count(r) == 1);
	/* What the OS produces for a closed loopback port is platform
	 * truth: POSIX stacks RST instantly (connection refused);
	 * Windows' winsock connect-retry cycle can eat the whole
	 * connect budget instead, surfacing as a connect timeout.  Both
	 * are legal HERE — but each must carry ITS OWN classification
	 * (refused = transient retry-same, connect timeout = advance);
	 * the mapping itself is pinned exactly in test_classify. */
	{
		const sicha_attempt *a = sicha_result_attempt(r, 0);
		int refused = a->transport_status == SICHA_T_E_CONNECT &&
			a->error_class == SICHA_CLASS_RETRY_SAME;
		int timed = a->transport_status ==
				SICHA_T_E_TIMEOUT_CONNECT &&
			a->error_class == SICHA_CLASS_ADVANCE;

		fprintf(stderr, "refused-port probe observed: %s / %s\n",
			sicha_transport_status_str(a->transport_status),
			sicha_error_class_str(a->error_class));
		T_CHECK(refused || timed);
	}
	sicha_result_destroy(r);
	sicha_client_destroy(client);
}

static void check_connection_reuse(void)
{
	/* two sequential requests must ride ONE connection (the pooled
	 * easy handle's cache) */
	t_http_server *srv = t_http_server_start();
	sicha_client *client;

	T_CHECK(srv != NULL);
	client = mk_client(t_http_server_port(srv), NULL, NULL);
	{
		t_http_response resp;

		memset(&resp, 0, sizeof(resp));
		resp.status = 200;
		resp.body = T_OK_BODY("one");
		resp.body_len = (size_t)-1;
		t_http_server_push(srv, &resp);
		resp.body = T_OK_BODY("two");
		t_http_server_push(srv, &resp);
	}
	for (int i = 0; i < 2; i++) {
		sicha_request req = t_req1();
		sicha_result *r = NULL;

		T_CHECK(sicha_chat(client, &req, NULL, NULL, &r) ==
			SICHA_OK);
		sicha_result_destroy(r);
	}
	{
		int conn0 = -1;
		int conn1 = -2;

		T_CHECK(t_http_server_request_count(srv) == 2);
		T_CHECK(t_http_server_request(srv, 0, &conn0) != NULL);
		T_CHECK(t_http_server_request(srv, 1, &conn1) != NULL);
		T_CHECK(conn0 == conn1);
	}
	sicha_client_destroy(client);
	t_http_server_stop(srv);
}

typedef struct trigger_arg {
	sicha_cancel *cancel;
	unsigned after_ms;
} trigger_arg;

static void trigger_after(void *p)
{
	trigger_arg *t = p;

	sicha_clock_real.wait_ms(NULL, NULL, t->after_ms);
	sicha_cancel_trigger(t->cancel);
}

static void check_cancel_mid_stream_real(void)
{
	/* the server dribbles a stream forever; a second thread pulls
	 * the token; unwind must be prompt */
	t_http_server *srv = t_http_server_start();
	sicha_client *client;
	sicha_result *r = NULL;
	sicha_cancel *cancel = sicha_cancel_create();
	sicha_thread th;
	trigger_arg ta;
	t_http_response resp;

	T_CHECK(srv != NULL);
	client = mk_client(t_http_server_port(srv), NULL, NULL);
	memset(&resp, 0, sizeof(resp));
	resp.status = 200;
	resp.body = T_SSE_BODY("drip", "feed");
	resp.body_len = (size_t)-1;
	resp.chunk_size = 4;
	resp.per_chunk_delay_ms = 400; /* ~14 s to finish naturally */
	t_http_server_push(srv, &resp);

	ta.cancel = cancel;
	ta.after_ms = 250;
	T_CHECK(sicha_thread_create(&th, trigger_after, &ta) == 1);
	{
		sicha_request req = t_req1();
		uint64_t t0 = sicha_clock_real.now_ms(NULL);

		T_CHECK(sicha_chat_stream(client, &req, NULL, cancel,
			&r) == SICHA_E_CANCELLED);
		T_CHECK(sicha_clock_real.now_ms(NULL) - t0 < 5000);
	}
	sicha_thread_join(th);
	T_CHECK(sicha_result_attempt_count(r) == 1);
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_CANCELLED);
	sicha_result_destroy(r);
	sicha_cancel_destroy(cancel);
	sicha_client_destroy(client);
	t_http_server_stop(srv);
}

int main(void)
{
	check_blocking_happy();
	check_streaming_happy();
	check_slow_headers_timeout();
	check_stalled_body_idle_timeout();
	check_mid_body_loss_retries();
	check_retry_after_over_the_wire();
	check_connection_refused();
	check_connection_reuse();
	check_cancel_mid_stream_real();
	return t_done("test_curl");
}
