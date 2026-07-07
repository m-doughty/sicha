/* M3b deliverables: the cancel token (semantics + real-clock wake
 * latency), the fake clock, and the scripted transport — including
 * all four timeout classes reproduced deterministically under
 * virtual time. */

#include <stdlib.h>

#include "sicha_internal.h"
#include "sicha_thread.h"
#include "support.h"

/* ------------------------------------------------------------------ */
/* Cancel token                                                        */
/* ------------------------------------------------------------------ */

static void check_cancel_token(void)
{
	sicha_cancel *c = sicha_cancel_create();

	T_CHECK(c != NULL);
	T_CHECK(sicha_cancel_is_cancelled(c) == 0);
	sicha_cancel_trigger(c);
	T_CHECK(sicha_cancel_is_cancelled(c) != 0);
	sicha_cancel_trigger(c); /* idempotent */
	T_CHECK(sicha_cancel_is_cancelled(c) != 0);
	sicha_cancel_reset(c);
	T_CHECK(sicha_cancel_is_cancelled(c) == 0);
	sicha_cancel_destroy(c);
	/* NULL-safe surface */
	sicha_cancel_destroy(NULL);
	sicha_cancel_trigger(NULL);
	sicha_cancel_reset(NULL);
	T_CHECK(sicha_cancel_is_cancelled(NULL) == 0);
}

static void trigger_soon(void *arg)
{
	sicha_cancel *c = arg;

	sicha_clock_real.wait_ms(NULL, NULL, 30);
	sicha_cancel_trigger(c);
}

static void check_real_clock(void)
{
	/* monotone non-decreasing */
	{
		uint64_t a = sicha_clock_real.now_ms(NULL);
		uint64_t b = sicha_clock_real.now_ms(NULL);

		T_CHECK(b >= a);
	}
	/* plain wait actually waits (loose lower bound) */
	{
		uint64_t a = sicha_clock_real.now_ms(NULL);

		T_CHECK(sicha_clock_real.wait_ms(NULL, NULL, 30) == 0);
		T_CHECK(sicha_clock_real.now_ms(NULL) - a >= 20);
	}
	/* pre-cancelled token returns immediately */
	{
		sicha_cancel *c = sicha_cancel_create();
		uint64_t a = sicha_clock_real.now_ms(NULL);

		sicha_cancel_trigger(c);
		T_CHECK(sicha_clock_real.wait_ms(NULL, c, 5000) != 0);
		T_CHECK(sicha_clock_real.now_ms(NULL) - a < 1000);
		sicha_cancel_destroy(c);
	}
	/* trigger from another thread wakes the wait early: the whole
	 * point of the condvar design.  Bounds stay loose for noisy CI
	 * boxes: a 10 s wait must end well under 5 s. */
	{
		sicha_cancel *c = sicha_cancel_create();
		sicha_thread th;
		uint64_t a = sicha_clock_real.now_ms(NULL);

		T_CHECK(sicha_thread_create(&th, trigger_soon, c) == 1);
		T_CHECK(sicha_clock_real.wait_ms(NULL, c, 10000) != 0);
		T_CHECK(sicha_clock_real.now_ms(NULL) - a < 5000);
		sicha_thread_join(th);
		sicha_cancel_destroy(c);
	}
	/* wait that expires without cancellation reports 0 */
	{
		sicha_cancel *c = sicha_cancel_create();

		T_CHECK(sicha_clock_real.wait_ms(NULL, c, 20) == 0);
		sicha_cancel_destroy(c);
	}
}

static void check_fake_clock(void)
{
	t_fake_clock fc;
	const sicha_clock *ck = t_fake_clock_init(&fc, 1000);

	T_CHECK(ck->now_ms(ck->ud) == 1000);
	T_CHECK(ck->wait_ms(ck->ud, NULL, 250) == 0);
	T_CHECK(ck->now_ms(ck->ud) == 1250);
	T_CHECK(fc.total_waited == 250);
	T_CHECK(fc.wait_calls == 1);
	/* trigger-on-wait fires the token and reports cancellation */
	{
		sicha_cancel *c = sicha_cancel_create();

		t_fake_clock_init(&fc, 0);
		fc.trigger_on_wait = c;
		fc.trigger_at_call = 2;
		T_CHECK(ck->wait_ms(ck->ud, c, 100) == 0);
		T_CHECK(ck->wait_ms(ck->ud, c, 100) != 0);
		T_CHECK(sicha_cancel_is_cancelled(c));
		/* virtual time only advanced for the completed wait */
		T_CHECK(fc.now == 100);
		sicha_cancel_destroy(c);
	}
}

/* ------------------------------------------------------------------ */
/* Scripted transport                                                  */
/* ------------------------------------------------------------------ */

typedef struct sink_rec {
	int32_t http_status;
	int n_status;
	char body[8192];
	size_t body_len;
	int n_body_calls;
	size_t body_call_sizes[64];
	char retry_after[64];
	int abort_on_status;
	int abort_after_body_calls;
} sink_rec;

static int32_t sink_on_status(void *ud, int32_t http_status,
	const sicha_header *headers, size_t n_headers)
{
	sink_rec *sr = ud;

	sr->http_status = http_status;
	sr->n_status++;
	for (size_t i = 0; i < n_headers; i++) {
		if (sicha_header_name_eq(headers[i].name, "Retry-After")) {
			snprintf(sr->retry_after, sizeof(sr->retry_after),
				"%s", headers[i].value);
		}
	}
	return sr->abort_on_status ? 1 : 0;
}

static int32_t sink_on_body(void *ud, const char *bytes, size_t len)
{
	sink_rec *sr = ud;

	memcpy(sr->body + sr->body_len, bytes, len);
	sr->body_len += len;
	if (sr->n_body_calls < 64) {
		sr->body_call_sizes[sr->n_body_calls] = len;
	}
	sr->n_body_calls++;
	if (sr->abort_after_body_calls != 0 &&
		sr->n_body_calls >= sr->abort_after_body_calls) {
		return 1;
	}
	return 0;
}

static sicha_http_sink mk_sink(sink_rec *sr)
{
	sicha_http_sink s = { sr, sink_on_status, sink_on_body };

	return s;
}

static sicha_http_request mk_req(void)
{
	sicha_http_request r;

	memset(&r, 0, sizeof(r));
	r.struct_size = (uint32_t)sizeof(r);
	r.method = "POST";
	r.url = "http://script.local/v1/chat/completions";
	r.body = "{\"x\":1}";
	r.body_len = 7;
	r.timeouts.connect_ms = 30000;
	r.timeouts.first_byte_ms = 60000;
	r.timeouts.idle_ms = 300000;
	r.timeouts.total_ms = 1800000;
	return r;
}

static sicha_script_response mk_resp(const char *body)
{
	sicha_script_response r;

	memset(&r, 0, sizeof(r));
	r.struct_size = (uint32_t)sizeof(r);
	r.status = SICHA_T_OK;
	r.http_status = 200;
	r.body = body;
	r.body_len = SICHA_LEN_CSTR;
	return r;
}

static void check_script_happy(void)
{
	sicha_transport *t = sicha_script_create();
	t_fake_clock fc;
	const sicha_clock *ck = t_fake_clock_init(&fc, 0);
	sink_rec sr;
	sicha_http_sink sink = mk_sink(&sr);
	sicha_http_request req = mk_req();
	sicha_header ra = { "Retry-After", "7" };
	sicha_script_response resp = mk_resp("hello body");

	memset(&sr, 0, sizeof(sr));
	resp.headers = &ra;
	resp.n_headers = 1;
	T_CHECK(sicha_script_push(t, &resp) == SICHA_OK);

	T_CHECK(t->perform(t->ud, &req, &sink, NULL, ck) == SICHA_T_OK);
	T_CHECK(sr.n_status == 1);
	T_CHECK(sr.http_status == 200);
	T_CHECK(strcmp(sr.retry_after, "7") == 0);
	T_CHECK(sr.body_len == 10 &&
		memcmp(sr.body, "hello body", 10) == 0);
	T_CHECK(sr.n_body_calls == 1); /* chunk_size 0 = single write */

	/* call recording */
	T_CHECK(sicha_script_call_count(t) == 1);
	T_CHECK(strcmp(sicha_script_call_url(t, 0), req.url) == 0);
	{
		size_t blen = 0;
		const char *b = sicha_script_call_body(t, 0, &blen);

		T_CHECK(b != NULL && blen == 7 &&
			memcmp(b, "{\"x\":1}", 7) == 0);
	}
	T_CHECK(sicha_script_call_url(t, 1) == NULL);
	T_CHECK(sicha_script_call_body(t, 9, NULL) == NULL);
	sicha_script_destroy(t);
}

static void check_script_call_headers(void)
{
	sicha_transport *t = sicha_script_create();
	t_fake_clock fc;
	const sicha_clock *ck = t_fake_clock_init(&fc, 0);
	sicha_http_request req = mk_req();
	sicha_header reqh[2] = {
		{ "Authorization", "Bearer sk-1" },
		{ "X-Title", "T" },
	};

	req.headers = reqh;
	req.n_headers = 2;
	/* empty queue: still recorded, answers CONNECT error */
	T_CHECK(t->perform(t->ud, &req, NULL, NULL, ck) ==
		SICHA_T_E_CONNECT);
	T_CHECK(sicha_script_call_count(t) == 1);
	T_CHECK(strcmp(sicha_script_call_header(t, 0, "authorization"),
		"Bearer sk-1") == 0);
	T_CHECK(strcmp(sicha_script_call_header(t, 0, "x-title"),
		"T") == 0);
	T_CHECK(sicha_script_call_header(t, 0, "Missing") == NULL);
	T_CHECK(sicha_script_call_header(t, 3, "X-Title") == NULL);
	sicha_script_destroy(t);
}

static void check_script_chunking(void)
{
	sicha_transport *t = sicha_script_create();
	t_fake_clock fc;
	const sicha_clock *ck = t_fake_clock_init(&fc, 0);
	sink_rec sr;
	sicha_http_sink sink = mk_sink(&sr);
	sicha_http_request req = mk_req();
	sicha_script_response resp = mk_resp("0123456789");

	memset(&sr, 0, sizeof(sr));
	resp.chunk_size = 3;
	T_CHECK(sicha_script_push(t, &resp) == SICHA_OK);
	T_CHECK(t->perform(t->ud, &req, &sink, NULL, ck) == SICHA_T_OK);
	T_CHECK(sr.n_body_calls == 4);
	T_CHECK(sr.body_call_sizes[0] == 3 && sr.body_call_sizes[3] == 1);
	T_CHECK(sr.body_len == 10 &&
		memcmp(sr.body, "0123456789", 10) == 0);
	sicha_script_destroy(t);
}

static void check_script_pure_error(void)
{
	sicha_transport *t = sicha_script_create();
	t_fake_clock fc;
	const sicha_clock *ck = t_fake_clock_init(&fc, 0);
	sink_rec sr;
	sicha_http_sink sink = mk_sink(&sr);
	sicha_http_request req = mk_req();
	sicha_script_response resp = mk_resp("ignored");

	memset(&sr, 0, sizeof(sr));
	resp.status = SICHA_T_E_RESET;
	T_CHECK(sicha_script_push(t, &resp) == SICHA_OK);
	T_CHECK(t->perform(t->ud, &req, &sink, NULL, ck) ==
		SICHA_T_E_RESET);
	T_CHECK(sr.n_status == 0);
	T_CHECK(sr.body_len == 0);
	sicha_script_destroy(t);
}

static void check_script_fail_after_bytes(void)
{
	sicha_transport *t = sicha_script_create();
	t_fake_clock fc;
	const sicha_clock *ck = t_fake_clock_init(&fc, 0);
	sink_rec sr;
	sicha_http_sink sink = mk_sink(&sr);
	sicha_http_request req = mk_req();
	sicha_script_response resp = mk_resp("0123456789");

	memset(&sr, 0, sizeof(sr));
	resp.status = SICHA_T_E_RESET;
	resp.fail_after_bytes = 4;
	resp.chunk_size = 2;
	T_CHECK(sicha_script_push(t, &resp) == SICHA_OK);
	T_CHECK(t->perform(t->ud, &req, &sink, NULL, ck) ==
		SICHA_T_E_RESET);
	T_CHECK(sr.n_status == 1);
	T_CHECK(sr.body_len == 4 && memcmp(sr.body, "0123", 4) == 0);
	sicha_script_destroy(t);
}

static void check_script_timeouts(void)
{
	/* connect timeout: delay 50 crosses budget 30 at t=30 */
	{
		sicha_transport *t = sicha_script_create();
		t_fake_clock fc;
		const sicha_clock *ck = t_fake_clock_init(&fc, 0);
		sicha_http_request req = mk_req();
		sicha_script_response resp = mk_resp("x");

		req.timeouts.connect_ms = 30;
		resp.connect_delay_ms = 50;
		T_CHECK(sicha_script_push(t, &resp) == SICHA_OK);
		T_CHECK(t->perform(t->ud, &req, NULL, NULL, ck) ==
			SICHA_T_E_TIMEOUT_CONNECT);
		T_CHECK(fc.now == 30);
		sicha_script_destroy(t);
	}
	/* first-byte timeout after a healthy connect */
	{
		sicha_transport *t = sicha_script_create();
		t_fake_clock fc;
		const sicha_clock *ck = t_fake_clock_init(&fc, 0);
		sink_rec sr;
		sicha_http_sink sink = mk_sink(&sr);
		sicha_http_request req = mk_req();
		sicha_script_response resp = mk_resp("x");

		memset(&sr, 0, sizeof(sr));
		req.timeouts.first_byte_ms = 60;
		resp.connect_delay_ms = 10;
		resp.first_byte_delay_ms = 500;
		T_CHECK(sicha_script_push(t, &resp) == SICHA_OK);
		T_CHECK(t->perform(t->ud, &req, &sink, NULL, ck) ==
			SICHA_T_E_TIMEOUT_FIRST_BYTE);
		T_CHECK(sr.n_status == 0);
		/* first_byte budget runs from perform start */
		T_CHECK(fc.now == 60);
		sicha_script_destroy(t);
	}
	/* idle timeout mid-body */
	{
		sicha_transport *t = sicha_script_create();
		t_fake_clock fc;
		const sicha_clock *ck = t_fake_clock_init(&fc, 0);
		sink_rec sr;
		sicha_http_sink sink = mk_sink(&sr);
		sicha_http_request req = mk_req();
		sicha_script_response resp = mk_resp("0123456789");

		memset(&sr, 0, sizeof(sr));
		req.timeouts.idle_ms = 200;
		resp.chunk_size = 5;
		resp.per_chunk_delay_ms = 500;
		T_CHECK(sicha_script_push(t, &resp) == SICHA_OK);
		T_CHECK(t->perform(t->ud, &req, &sink, NULL, ck) ==
			SICHA_T_E_TIMEOUT_IDLE);
		T_CHECK(sr.n_status == 1);
		T_CHECK(sr.body_len == 0); /* died before chunk 1 */
		sicha_script_destroy(t);
	}
	/* total timeout wins across phases */
	{
		sicha_transport *t = sicha_script_create();
		t_fake_clock fc;
		const sicha_clock *ck = t_fake_clock_init(&fc, 0);
		sink_rec sr;
		sicha_http_sink sink = mk_sink(&sr);
		sicha_http_request req = mk_req();
		sicha_script_response resp = mk_resp("0123456789");

		memset(&sr, 0, sizeof(sr));
		req.timeouts.total_ms = 100;
		resp.connect_delay_ms = 20;
		resp.first_byte_delay_ms = 20;
		resp.chunk_size = 2;
		/* chunk 1 lands at t=70; chunk 2's delay reaches the
		 * total deadline exactly at t=100 and ties go to the
		 * timeout, so only one chunk is ever delivered */
		resp.per_chunk_delay_ms = 30;
		T_CHECK(sicha_script_push(t, &resp) == SICHA_OK);
		T_CHECK(t->perform(t->ud, &req, &sink, NULL, ck) ==
			SICHA_T_E_TIMEOUT_TOTAL);
		T_CHECK(sr.body_len == 2);
		T_CHECK(fc.now == 100);
		sicha_script_destroy(t);
	}
	/* INFINITE first_byte: a very long prefill completes fine */
	{
		sicha_transport *t = sicha_script_create();
		t_fake_clock fc;
		const sicha_clock *ck = t_fake_clock_init(&fc, 0);
		sink_rec sr;
		sicha_http_sink sink = mk_sink(&sr);
		sicha_http_request req = mk_req();
		sicha_script_response resp = mk_resp("done");

		memset(&sr, 0, sizeof(sr));
		req.timeouts.first_byte_ms = SICHA_INFINITE;
		req.timeouts.total_ms = SICHA_INFINITE;
		resp.first_byte_delay_ms = 10ull * 60u * 1000u;
		T_CHECK(sicha_script_push(t, &resp) == SICHA_OK);
		T_CHECK(t->perform(t->ud, &req, &sink, NULL, ck) ==
			SICHA_T_OK);
		T_CHECK(sr.body_len == 4);
		sicha_script_destroy(t);
	}
}

static void check_script_cancellation(void)
{
	sicha_transport *t = sicha_script_create();
	t_fake_clock fc;
	const sicha_clock *ck = t_fake_clock_init(&fc, 0);
	sicha_cancel *c = sicha_cancel_create();
	sicha_http_request req = mk_req();
	sicha_script_response resp = mk_resp("x");

	/* cancellation fires during the connect delay */
	resp.connect_delay_ms = 5000;
	fc.trigger_on_wait = c;
	fc.trigger_at_call = 3;
	T_CHECK(sicha_script_push(t, &resp) == SICHA_OK);
	T_CHECK(t->perform(t->ud, &req, NULL, c, ck) ==
		SICHA_T_E_CANCELLED);
	sicha_cancel_destroy(c);
	sicha_script_destroy(t);
}

static void check_script_sink_abort(void)
{
	sicha_transport *t = sicha_script_create();
	t_fake_clock fc;
	const sicha_clock *ck = t_fake_clock_init(&fc, 0);
	sink_rec sr;
	sicha_http_sink sink = mk_sink(&sr);
	sicha_http_request req = mk_req();
	sicha_script_response resp = mk_resp("0123456789");

	/* abort from on_status */
	memset(&sr, 0, sizeof(sr));
	sr.abort_on_status = 1;
	T_CHECK(sicha_script_push(t, &resp) == SICHA_OK);
	T_CHECK(t->perform(t->ud, &req, &sink, NULL, ck) ==
		SICHA_T_E_ABORTED_BY_SINK);
	T_CHECK(sr.body_len == 0);

	/* abort mid-body */
	memset(&sr, 0, sizeof(sr));
	sr.abort_after_body_calls = 2;
	resp.chunk_size = 3;
	T_CHECK(sicha_script_push(t, &resp) == SICHA_OK);
	T_CHECK(t->perform(t->ud, &req, &sink, NULL, ck) ==
		SICHA_T_E_ABORTED_BY_SINK);
	T_CHECK(sr.body_len == 6);
	sicha_script_destroy(t);
}

static void check_script_push_validation(void)
{
	sicha_transport *t = sicha_script_create();
	sicha_script_response resp = mk_resp("x");

	T_CHECK(sicha_script_push(NULL, &resp) == SICHA_E_INVALID_ARG);
	T_CHECK(sicha_script_push(t, NULL) == SICHA_E_INVALID_ARG);
	resp.struct_size = 4;
	T_CHECK(sicha_script_push(t, &resp) == SICHA_E_INVALID_ARG);
	resp = mk_resp("x");
	resp.n_headers = SICHA_MAX_HEADERS + 1;
	T_CHECK(sicha_script_push(t, &resp) == SICHA_E_INVALID_ARG);
	resp = mk_resp("x");
	resp.n_headers = 2;
	resp.headers = NULL;
	T_CHECK(sicha_script_push(t, &resp) == SICHA_E_INVALID_ARG);
	{
		char big[SICHA_MAX_HEADER_BYTES + 8];
		sicha_header h = { "N", big };

		memset(big, 'v', sizeof(big) - 1);
		big[sizeof(big) - 1] = '\0';
		resp = mk_resp("x");
		resp.headers = &h;
		resp.n_headers = 1;
		T_CHECK(sicha_script_push(t, &resp) ==
			SICHA_E_INVALID_ARG);
	}
	sicha_script_destroy(t);
	sicha_script_destroy(NULL);
}

int main(void)
{
	check_cancel_token();
	check_real_clock();
	check_fake_clock();
	check_script_happy();
	check_script_call_headers();
	check_script_chunking();
	check_script_pure_error();
	check_script_fail_after_bytes();
	check_script_timeouts();
	check_script_cancellation();
	check_script_sink_abort();
	check_script_push_validation();
	return t_done("test_script");
}
