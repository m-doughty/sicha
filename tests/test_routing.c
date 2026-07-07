/* The routing state machine: every transition (success, retry-same
 * chains, budget exhaustion, advance, abort, validation), exhaustion
 * record assertions, per-backend body/header rebuilds, telemetry
 * ordering, streaming parity, and the streaming commit rule. */

#include <stdlib.h>

#include "engine_helpers.h"

/* One complete delta event, then the connection dies: the caller SAW
 * "partial" before the failure. */
static const char T_PARTIAL_SSE[] =
	"data: {\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}\n\n";

/* on_attempt recorder */
typedef struct att_rec {
	uint32_t ordinals[32];
	sicha_error_class classes[32];
	int n;
} att_rec;

static void on_attempt_rec(void *ud, const sicha_attempt *a)
{
	att_rec *ar = ud;

	if (ar->n < 32) {
		ar->ordinals[ar->n] = a->attempt;
		ar->classes[ar->n] = a->error_class;
	}
	ar->n++;
}

static sicha_callbacks cbs_with_attempts(att_rec *ar)
{
	sicha_callbacks cbs;

	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = ar;
	cbs.on_attempt = on_attempt_rec;
	return cbs;
}

static void check_happy_path(void)
{
	t_env e;
	sicha_result *r = NULL;
	att_rec ar = { { 0 }, { 0 }, 0 };
	sicha_callbacks cbs = cbs_with_attempts(&ar);

	T_CHECK(t_env_init(&e, 2, NULL));
	t_push_ok(e.script, T_OK_BODY("hello"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, &cbs, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(sicha_result_status(r) == SICHA_OK);
	T_CHECK(strcmp(sicha_result_text(r, NULL), "hello") == 0);
	T_CHECK(sicha_result_backend(r) == 0);
	T_CHECK(strcmp(sicha_result_model(r), "served") == 0);
	T_CHECK(sicha_result_prompt_tokens(r) == 5);
	T_CHECK(sicha_result_attempt_count(r) == 1);
	{
		const sicha_attempt *a = sicha_result_attempt(r, 0);

		T_CHECK(a->error_class == SICHA_CLASS_NONE);
		T_CHECK(a->http_status == 200);
		T_CHECK(strcmp(a->model, "m0") == 0);
		T_CHECK(strcmp(a->message, "ok") == 0);
		T_CHECK(a->raw_body_excerpt_len == 0);
		T_CHECK(a->prompt_tokens == 5 && a->total_tokens == 7);
	}
	T_CHECK(ar.n == 1);
	/* the second backend was never touched */
	T_CHECK(sicha_script_call_count(e.script) == 1);
	/* raw body retained verbatim on non-streaming calls */
	{
		size_t blen = 0;
		const char *raw = sicha_result_raw_body(r, &blen);

		T_CHECK(blen == strlen(T_OK_BODY("hello")));
		T_CHECK(strcmp(raw, T_OK_BODY("hello")) == 0);
	}
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_retry_chain_then_success(void)
{
	t_env e;
	sicha_result *r = NULL;

	T_CHECK(t_env_init(&e, 1, NULL)); /* max_tries default 3 */
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_http(e.script, 500, "boom", NULL, 0);
	t_push_ok(e.script, T_OK_BODY("third time lucky"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(sicha_result_attempt_count(r) == 3);
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_RETRY_SAME);
	T_CHECK(sicha_result_attempt(r, 0)->transport_status ==
		SICHA_T_E_RESET);
	T_CHECK(sicha_result_attempt(r, 1)->error_class ==
		SICHA_CLASS_RETRY_SAME);
	T_CHECK(sicha_result_attempt(r, 1)->http_status == 500);
	T_CHECK(sicha_result_attempt(r, 2)->error_class ==
		SICHA_CLASS_NONE);
	T_CHECK(sicha_result_attempt(r, 0)->try_of_backend == 0);
	T_CHECK(sicha_result_attempt(r, 1)->try_of_backend == 1);
	T_CHECK(sicha_result_attempt(r, 2)->try_of_backend == 2);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_budget_exhaustion_advances(void)
{
	t_env e;
	sicha_result *r = NULL;

	T_CHECK(t_env_init(&e, 2, NULL));
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_terr(e.script, SICHA_T_E_RESET); /* 3rd = budget gone */
	t_push_ok(e.script, T_OK_BODY("fallback"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(strcmp(sicha_result_text(r, NULL), "fallback") == 0);
	T_CHECK(sicha_result_backend(r) == 1);
	T_CHECK(sicha_result_attempt_count(r) == 4);
	T_CHECK(sicha_result_attempt(r, 2)->backend == 0);
	T_CHECK(sicha_result_attempt(r, 3)->backend == 1);
	T_CHECK(sicha_result_attempt(r, 3)->try_of_backend == 0);
	/* the fallback request carried backend 1's model and url */
	{
		size_t blen = 0;
		const char *body = sicha_script_call_body(e.script, 3,
			&blen);

		T_CHECK(body != NULL &&
			strstr(body, "\"model\":\"m1\"") != NULL);
		T_CHECK(strcmp(sicha_script_call_url(e.script, 3),
			"http://b1.local/v1/chat/completions") == 0);
	}
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_abort_stops_the_chain(void)
{
	t_env e;
	sicha_result *r = NULL;

	T_CHECK(t_env_init(&e, 2, NULL));
	t_push_http(e.script, 401,
		"{\"error\":{\"message\":\"bad key\"}}", NULL, 0);
	t_push_ok(e.script, T_OK_BODY("never"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_E_ABORTED);
	}
	T_CHECK(sicha_result_status(r) == SICHA_E_ABORTED);
	T_CHECK(sicha_result_attempt_count(r) == 1);
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_ABORT);
	T_CHECK(sicha_script_call_count(e.script) == 1);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_429_advances_immediately(void)
{
	t_env e;
	sicha_result *r = NULL;

	T_CHECK(t_env_init(&e, 2, NULL));
	t_push_http(e.script, 429, "rate limited", NULL, 0);
	t_push_ok(e.script, T_OK_BODY("next backend"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(sicha_result_backend(r) == 1);
	T_CHECK(e.fc.wait_calls == 0); /* advance never backs off */
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_ADVANCE);
	/* advance-class failures carry the body excerpt */
	T_CHECK(sicha_result_attempt(r, 0)->raw_body_excerpt_len ==
		strlen("rate limited"));
	T_CHECK(strcmp(sicha_result_attempt(r, 0)->raw_body_excerpt,
		"rate limited") == 0);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_timeout_and_body_advances(void)
{
	t_env e;
	sicha_result *r = NULL;

	T_CHECK(t_env_init(&e, 3, NULL));
	t_push_terr(e.script, SICHA_T_E_TIMEOUT_FIRST_BYTE);
	t_push_http(e.script, 200, "", NULL, 0);          /* empty     */
	t_push_ok(e.script, T_OK_BODY("c"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(sicha_result_backend(r) == 2);
	T_CHECK(sicha_result_attempt_count(r) == 3);
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_ADVANCE);
	T_CHECK(sicha_result_attempt(r, 1)->error_class ==
		SICHA_CLASS_ADVANCE);
	T_CHECK(strcmp(sicha_result_attempt(r, 1)->message,
		"empty response body") == 0);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_malformed_body_excerpt(void)
{
	t_env e;
	sicha_result *r = NULL;

	T_CHECK(t_env_init(&e, 1, NULL));
	t_push_http(e.script, 200, "<html>gateway melted</html>", NULL, 0);
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_E_EXHAUSTED);
	}
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_ADVANCE);
	T_CHECK(strcmp(sicha_result_attempt(r, 0)->raw_body_excerpt,
		"<html>gateway melted</html>") == 0);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_finish_reason_policies(void)
{
	static const char body[] = "{\"choices\":[{\"message\":"
		"{\"content\":\"trunca\"},\"finish_reason\":\"length\"}]}";

	/* default: truncation is success */
	{
		t_env e;
		sicha_result *r = NULL;

		T_CHECK(t_env_init(&e, 1, NULL));
		t_push_http(e.script, 200, body, NULL, 0);
		{
			sicha_request req = t_req1();

			T_CHECK(sicha_chat(e.client, &req, NULL, NULL,
				&r) == SICHA_OK);
		}
		T_CHECK(sicha_result_finish_reason(r) ==
			SICHA_FINISH_LENGTH);
		sicha_result_destroy(r);
		t_env_free(&e);
	}
	/* pipeline mode: LENGTH_IS_ADVANCE */
	{
		sicha_retry_policy pol;
		t_env e;
		sicha_result *r = NULL;

		memset(&pol, 0, sizeof(pol));
		pol.flags = SICHA_POLICY_LENGTH_IS_ADVANCE;
		T_CHECK(t_env_init(&e, 1, &pol));
		t_push_http(e.script, 200, body, NULL, 0);
		{
			sicha_request req = t_req1();

			T_CHECK(sicha_chat(e.client, &req, NULL, NULL,
				&r) == SICHA_E_EXHAUSTED);
		}
		T_CHECK(sicha_result_attempt(r, 0)->error_class ==
			SICHA_CLASS_ADVANCE);
		sicha_result_destroy(r);
		t_env_free(&e);
	}
}

static void check_full_exhaustion_records(void)
{
	sicha_retry_policy pol;
	t_env e;
	sicha_result *r = NULL;
	att_rec ar = { { 0 }, { 0 }, 0 };
	sicha_callbacks cbs = cbs_with_attempts(&ar);

	memset(&pol, 0, sizeof(pol));
	pol.max_tries = 2;
	T_CHECK(t_env_init(&e, 2, &pol));
	/* backend 0: reset, 502; backend 1: 502, 502 */
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_http(e.script, 502, "{\"error\":\"bad gateway\"}", NULL, 0);
	t_push_http(e.script, 502, "b1-fail-1", NULL, 0);
	t_push_http(e.script, 502, "b1-fail-2", NULL, 0);
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, &cbs, NULL, &r) ==
			SICHA_E_EXHAUSTED);
	}
	T_CHECK(sicha_result_status(r) == SICHA_E_EXHAUSTED);
	T_CHECK(sicha_result_attempt_count(r) == 4);
	/* attempt-budget invariant */
	T_CHECK(sicha_result_attempt_count(r) <=
		2 * (pol.max_tries + pol.validation_retries));
	{
		static const uint32_t backends[4] = { 0, 0, 1, 1 };
		static const uint32_t tries[4] = { 0, 1, 0, 1 };

		for (uint32_t i = 0; i < 4; i++) {
			const sicha_attempt *a = sicha_result_attempt(r, i);

			T_CHECK(a->attempt == i);
			T_CHECK(a->backend == backends[i]);
			T_CHECK(a->try_of_backend == tries[i]);
			T_CHECK(a->error_class == SICHA_CLASS_RETRY_SAME);
			T_CHECK(strcmp(a->model,
				a->backend == 0 ? "m0" : "m1") == 0);
		}
	}
	/* telemetry fired once per round trip, in order */
	T_CHECK(ar.n == 4);
	T_CHECK(ar.ordinals[0] == 0 && ar.ordinals[3] == 3);
	/* final-attempt backend reported */
	T_CHECK(sicha_result_backend(r) == 1);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_per_backend_credentials(void)
{
	sicha_backend_desc descs[2];
	sicha_client_opts opts;
	sicha_client *client = NULL;
	sicha_transport *script = sicha_script_create();
	t_fake_clock fc;
	sicha_result *r = NULL;

	descs[0] = t_backend("http://b0.local/v1", "m0");
	descs[0].api_key = "sk-zero";
	descs[1] = t_backend("http://b1.local/v1", "m1");
	descs[1].api_key = "sk-one";
	memset(&opts, 0, sizeof(opts));
	opts.struct_size = (uint32_t)sizeof(opts);
	opts.backends = descs;
	opts.n_backends = 2;
	opts.transport = script;
	opts.clock = t_fake_clock_init(&fc, 0);
	opts.prng_seed = 42;
	T_CHECK(sicha_client_create(&opts, &client) == SICHA_OK);

	t_push_http(script, 429, "advance", NULL, 0);
	t_push_ok(script, T_OK_BODY("ok"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(strcmp(sicha_script_call_header(script, 0,
		"Authorization"), "Bearer sk-zero") == 0);
	T_CHECK(strcmp(sicha_script_call_header(script, 1,
		"Authorization"), "Bearer sk-one") == 0);
	T_CHECK(strcmp(sicha_script_call_header(script, 0,
		"Content-Type"), "application/json") == 0);
	sicha_result_destroy(r);
	sicha_client_destroy(client);
	sicha_script_destroy(script);
}

/* ------------------------------------------------------------------ */
/* Streaming                                                           */
/* ------------------------------------------------------------------ */

typedef struct delta_rec {
	char text[512];
	size_t len;
	int resets; /* bumped from on_attempt: caller-side reset signal */
} delta_rec;

static int32_t stream_delta(void *ud, const char *bytes, size_t len)
{
	delta_rec *dr = ud;

	memcpy(dr->text + dr->len, bytes, len);
	dr->len += len;
	return 0;
}

static void stream_attempt(void *ud, const sicha_attempt *a)
{
	delta_rec *dr = ud;

	/* the documented RETRY_AFTER_DELTAS pattern: a non-NONE record
	 * means a retry may follow — discard accumulated state.  The
	 * final successful record must NOT wipe the finished text. */
	if (a->error_class != SICHA_CLASS_NONE) {
		dr->len = 0;
	}
	dr->resets++;
}

static void check_stream_happy(void)
{
	t_env e;
	sicha_result *r = NULL;
	delta_rec dr;
	sicha_callbacks cbs;

	memset(&dr, 0, sizeof(dr));
	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = &dr;
	cbs.on_delta = stream_delta;

	T_CHECK(t_env_init(&e, 1, NULL));
	t_push_ok(e.script, T_SSE_BODY("Hel", "lo"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat_stream(e.client, &req, &cbs, NULL,
			&r) == SICHA_OK);
	}
	T_CHECK(dr.len == 5 && memcmp(dr.text, "Hello", 5) == 0);
	T_CHECK(strcmp(sicha_result_text(r, NULL), "Hello") == 0);
	T_CHECK(sicha_result_finish_reason(r) == SICHA_FINISH_STOP);
	T_CHECK(strcmp(sicha_result_model(r), "served") == 0);
	/* streaming keeps no raw body */
	{
		size_t blen = 7;

		T_CHECK(strcmp(sicha_result_raw_body(r, &blen), "") == 0);
		T_CHECK(blen == 0);
	}
	/* the request asked for a stream */
	{
		size_t blen = 0;
		const char *body = sicha_script_call_body(e.script, 0,
			&blen);

		T_CHECK(strstr(body, "\"stream\":true") != NULL);
	}
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_stream_retry_before_deltas(void)
{
	/* failures before any delivery retry normally, even with a
	 * delta callback registered */
	t_env e;
	sicha_result *r = NULL;
	delta_rec dr;
	sicha_callbacks cbs;

	memset(&dr, 0, sizeof(dr));
	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = &dr;
	cbs.on_delta = stream_delta;

	T_CHECK(t_env_init(&e, 1, NULL));
	t_push_terr(e.script, SICHA_T_E_CONNECT);
	t_push_ok(e.script, T_SSE_BODY("second", " try"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat_stream(e.client, &req, &cbs, NULL,
			&r) == SICHA_OK);
	}
	T_CHECK(strcmp(sicha_result_text(r, NULL), "second try") == 0);
	T_CHECK(sicha_result_attempt_count(r) == 2);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_stream_lost_after_deltas(void)
{
	/* deltas delivered, then the stream dies: default policy must
	 * NOT silently retry */
	t_env e;
	sicha_result *r = NULL;
	delta_rec dr;
	sicha_callbacks cbs;

	memset(&dr, 0, sizeof(dr));
	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = &dr;
	cbs.on_delta = stream_delta;

	T_CHECK(t_env_init(&e, 1, NULL));
	t_push_broken_body(e.script, T_PARTIAL_SSE,
		sizeof(T_PARTIAL_SSE) - 1, SICHA_T_E_RESET);
	t_push_ok(e.script, T_SSE_BODY("never", "delivered"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat_stream(e.client, &req, &cbs, NULL,
			&r) == SICHA_E_STREAM_LOST);
	}
	T_CHECK(sicha_result_status(r) == SICHA_E_STREAM_LOST);
	T_CHECK(dr.len == 7 && memcmp(dr.text, "partial", 7) == 0);
	T_CHECK(sicha_result_attempt_count(r) == 1);
	T_CHECK(sicha_script_call_count(e.script) == 1); /* no retry */
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_stream_retry_after_deltas_opt_in(void)
{
	sicha_retry_policy pol;
	t_env e;
	sicha_result *r = NULL;
	delta_rec dr;
	sicha_callbacks cbs;

	memset(&pol, 0, sizeof(pol));
	pol.flags = SICHA_POLICY_RETRY_AFTER_DELTAS;
	pol.backoff_jitter_ms = SICHA_DISABLED;
	memset(&dr, 0, sizeof(dr));
	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = &dr;
	cbs.on_delta = stream_delta;
	cbs.on_attempt = stream_attempt;

	T_CHECK(t_env_init(&e, 1, &pol));
	t_push_broken_body(e.script, T_PARTIAL_SSE,
		sizeof(T_PARTIAL_SSE) - 1, SICHA_T_E_RESET);
	t_push_ok(e.script, T_SSE_BODY("full", " text"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat_stream(e.client, &req, &cbs, NULL,
			&r) == SICHA_OK);
	}
	/* on_attempt reset the caller's buffer between attempts */
	T_CHECK(dr.resets == 2);
	T_CHECK(dr.len == 9 && memcmp(dr.text, "full text", 9) == 0);
	T_CHECK(strcmp(sicha_result_text(r, NULL), "full text") == 0);
	T_CHECK(sicha_result_attempt_count(r) == 2);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_stream_accumulation_only_retries(void)
{
	/* no callbacks: partial accumulation is invisible to the
	 * caller, so retries stay safe under the DEFAULT policy and
	 * the result contains only the final attempt's text */
	t_env e;
	sicha_result *r = NULL;

	T_CHECK(t_env_init(&e, 1, NULL));
	t_push_broken_body(e.script, T_PARTIAL_SSE,
		sizeof(T_PARTIAL_SSE) - 1, SICHA_T_E_RESET);
	t_push_ok(e.script, T_SSE_BODY("clean", " result"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat_stream(e.client, &req, NULL, NULL,
			&r) == SICHA_OK);
	}
	T_CHECK(strcmp(sicha_result_text(r, NULL), "clean result") == 0);
	T_CHECK(sicha_result_attempt_count(r) == 2);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_stream_incomplete_advances(void)
{
	/* 200 + clean close with neither [DONE] nor finish_reason is a
	 * malformed stream: advance-class */
	t_env e;
	sicha_result *r = NULL;

	T_CHECK(t_env_init(&e, 1, NULL));
	t_push_http(e.script, 200,
		"data: {\"choices\":[{\"delta\":{\"content\":"
		"\"cut\"}}]}\n\n", NULL, 0);
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat_stream(e.client, &req, NULL, NULL,
			&r) == SICHA_E_EXHAUSTED);
	}
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_ADVANCE);
	T_CHECK(strcmp(sicha_result_attempt(r, 0)->message,
		"malformed or incomplete response") == 0);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_stream_finish_without_done_is_accepted(void)
{
	/* some gateways close without [DONE]; a finish_reason makes
	 * the stream complete (lenient EOF) */
	t_env e;
	sicha_result *r = NULL;

	T_CHECK(t_env_init(&e, 1, NULL));
	t_push_http(e.script, 200,
		"data: {\"choices\":[{\"delta\":{\"content\":\"all\"},"
		"\"finish_reason\":\"stop\"}]}\n\n", NULL, 0);
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat_stream(e.client, &req, NULL, NULL,
			&r) == SICHA_OK);
	}
	T_CHECK(strcmp(sicha_result_text(r, NULL), "all") == 0);
	sicha_result_destroy(r);
	t_env_free(&e);
}

int main(void)
{
	check_happy_path();
	check_retry_chain_then_success();
	check_budget_exhaustion_advances();
	check_abort_stops_the_chain();
	check_429_advances_immediately();
	check_timeout_and_body_advances();
	check_malformed_body_excerpt();
	check_finish_reason_policies();
	check_full_exhaustion_records();
	check_per_backend_credentials();
	check_stream_happy();
	check_stream_retry_before_deltas();
	check_stream_lost_after_deltas();
	check_stream_retry_after_deltas_opt_in();
	check_stream_accumulation_only_retries();
	check_stream_incomplete_advances();
	check_stream_finish_without_done_is_accepted();
	return t_done("test_routing");
}
