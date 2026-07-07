/* Engine cancellation at every phase: pre-flight, during backoff
 * (instant condvar wake), mid-stream via the transport, from a
 * callback return value — plus the KoboldCpp abort assist and token
 * reset/reuse. */

#include <stdlib.h>

#include "engine_helpers.h"

static void check_precancelled(void)
{
	t_env e;
	sicha_result *r = NULL;
	sicha_cancel *c = sicha_cancel_create();

	T_CHECK(t_env_init(&e, 2, NULL));
	t_push_ok(e.script, T_OK_BODY("never"));
	sicha_cancel_trigger(c);
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, c, &r) ==
			SICHA_E_CANCELLED);
	}
	T_CHECK(sicha_result_status(r) == SICHA_E_CANCELLED);
	T_CHECK(sicha_result_attempt_count(r) == 0);
	T_CHECK(sicha_result_backend(r) == -1);
	T_CHECK(sicha_script_call_count(e.script) == 0);
	sicha_result_destroy(r);
	sicha_cancel_destroy(c);
	t_env_free(&e);
}

static void check_cancel_during_backoff(void)
{
	t_env e;
	sicha_result *r = NULL;
	sicha_cancel *c;

	T_CHECK(t_env_init(&e, 1, NULL));
	c = sicha_cancel_create();
	/* first attempt fails retryably; the backoff wait is where the
	 * trigger lands (fake clock trigger hook) */
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_ok(e.script, T_OK_BODY("never"));
	e.fc.trigger_on_wait = c;
	e.fc.trigger_at_call = 1;
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, c, &r) ==
			SICHA_E_CANCELLED);
	}
	/* one attempt record (the RESET); the cancelled backoff adds
	 * no phantom record */
	T_CHECK(sicha_result_attempt_count(r) == 1);
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_RETRY_SAME);
	T_CHECK(sicha_script_call_count(e.script) == 1);
	sicha_result_destroy(r);
	sicha_cancel_destroy(c);
	t_env_free(&e);
}

static void check_cancel_mid_transport(void)
{
	t_env e;
	sicha_result *r = NULL;
	sicha_cancel *c;
	sicha_script_response resp;

	T_CHECK(t_env_init(&e, 1, NULL));
	c = sicha_cancel_create();
	memset(&resp, 0, sizeof(resp));
	resp.struct_size = (uint32_t)sizeof(resp);
	resp.status = SICHA_T_OK;
	resp.http_status = 200;
	resp.body = T_SSE_BODY("a", "b");
	resp.body_len = SICHA_LEN_CSTR;
	resp.connect_delay_ms = 5000; /* plenty of waits to land in */
	T_CHECK(sicha_script_push(e.script, &resp) == SICHA_OK);
	e.fc.trigger_on_wait = c;
	e.fc.trigger_at_call = 2;
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat_stream(e.client, &req, NULL, c, &r) ==
			SICHA_E_CANCELLED);
	}
	T_CHECK(sicha_result_attempt_count(r) == 1);
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_CANCELLED);
	T_CHECK(sicha_result_attempt(r, 0)->transport_status ==
		SICHA_T_E_CANCELLED);
	sicha_result_destroy(r);
	sicha_cancel_destroy(c);
	t_env_free(&e);
}

static int32_t cancel_on_second_delta(void *ud, const char *bytes,
	size_t len)
{
	int *count = ud;

	(void)bytes;
	(void)len;
	return ++(*count) >= 2 ? 1 : 0;
}

static void check_cancel_from_callback(void)
{
	t_env e;
	sicha_result *r = NULL;
	sicha_callbacks cbs;
	int deltas = 0;

	T_CHECK(t_env_init(&e, 1, NULL));
	t_push_ok(e.script, T_SSE_BODY("one", "two"));
	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = &deltas;
	cbs.on_delta = cancel_on_second_delta;
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat_stream(e.client, &req, &cbs, NULL,
			&r) == SICHA_E_CANCELLED);
	}
	T_CHECK(deltas == 2);
	T_CHECK(sicha_result_attempt_count(r) == 1);
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_CANCELLED);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_kobold_abort_assist(void)
{
	/* backend 0 carries the assist flag: a mid-transport cancel
	 * must fire POST {origin}/api/extra/abort, fire-and-forget */
	t_env e;
	sicha_result *r = NULL;
	sicha_cancel *c;
	sicha_script_response resp;

	T_CHECK(t_env_init_ex(&e, 1, NULL, 42,
		SICHA_BACKEND_KOBOLD_CANCEL_ASSIST));
	c = sicha_cancel_create();
	memset(&resp, 0, sizeof(resp));
	resp.struct_size = (uint32_t)sizeof(resp);
	resp.status = SICHA_T_OK;
	resp.http_status = 200;
	resp.body = T_SSE_BODY("a", "b");
	resp.body_len = SICHA_LEN_CSTR;
	resp.connect_delay_ms = 5000;
	T_CHECK(sicha_script_push(e.script, &resp) == SICHA_OK);
	e.fc.trigger_on_wait = c;
	e.fc.trigger_at_call = 1;
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat_stream(e.client, &req, NULL, c, &r) ==
			SICHA_E_CANCELLED);
	}
	/* call 0 = the chat attempt, call 1 = the abort assist */
	T_CHECK(sicha_script_call_count(e.script) == 2);
	T_CHECK(strcmp(sicha_script_call_url(e.script, 1),
		"http://b0.local/api/extra/abort") == 0);
	{
		size_t blen = 0;
		const char *b = sicha_script_call_body(e.script, 1, &blen);

		T_CHECK(blen == 2 && memcmp(b, "{}", 2) == 0);
	}
	sicha_result_destroy(r);
	sicha_cancel_destroy(c);
	t_env_free(&e);
}

static void check_no_assist_without_flag(void)
{
	t_env e;
	sicha_result *r = NULL;
	sicha_cancel *c;
	sicha_script_response resp;

	T_CHECK(t_env_init(&e, 1, NULL)); /* no flag */
	c = sicha_cancel_create();
	memset(&resp, 0, sizeof(resp));
	resp.struct_size = (uint32_t)sizeof(resp);
	resp.status = SICHA_T_OK;
	resp.http_status = 200;
	resp.body = T_SSE_BODY("a", "b");
	resp.body_len = SICHA_LEN_CSTR;
	resp.connect_delay_ms = 5000;
	T_CHECK(sicha_script_push(e.script, &resp) == SICHA_OK);
	e.fc.trigger_on_wait = c;
	e.fc.trigger_at_call = 1;
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat_stream(e.client, &req, NULL, c, &r) ==
			SICHA_E_CANCELLED);
	}
	T_CHECK(sicha_script_call_count(e.script) == 1);
	sicha_result_destroy(r);
	sicha_cancel_destroy(c);
	t_env_free(&e);
}

static void check_token_reset_and_reuse(void)
{
	t_env e;
	sicha_cancel *c = sicha_cancel_create();

	T_CHECK(t_env_init(&e, 1, NULL));
	t_push_ok(e.script, T_OK_BODY("after reset"));

	sicha_cancel_trigger(c);
	{
		sicha_request req = t_req1();
		sicha_result *r = NULL;

		T_CHECK(sicha_chat(e.client, &req, NULL, c, &r) ==
			SICHA_E_CANCELLED);
		sicha_result_destroy(r);
	}
	sicha_cancel_reset(c);
	{
		sicha_request req = t_req1();
		sicha_result *r = NULL;

		T_CHECK(sicha_chat(e.client, &req, NULL, c, &r) ==
			SICHA_OK);
		T_CHECK(strcmp(sicha_result_text(r, NULL),
			"after reset") == 0);
		sicha_result_destroy(r);
	}
	sicha_cancel_destroy(c);
	t_env_free(&e);
}

int main(void)
{
	check_precancelled();
	check_cancel_during_backoff();
	check_cancel_mid_transport();
	check_cancel_from_callback();
	check_kobold_abort_assist();
	check_no_assist_without_flag();
	check_token_reset_and_reuse();
	return t_done("test_cancel");
}
