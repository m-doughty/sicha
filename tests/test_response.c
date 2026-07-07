/* response.c: non-streaming body parse (goldens, every finish reason,
 * reasoning positions, tool-call turns, malformed-body taxonomy),
 * streaming assembly (delta accumulation + delivery, interleaved
 * reasoning, late finish_reason, usage chunks, [DONE], raw-chunk
 * forwarding, NO_ACCUMULATE, cancel-from-callback), and result
 * object semantics (attempt deep copies, reset, inert NULL getters). */

#include <stdlib.h>

#include "sicha_internal.h"
#include "support.h"

/* ------------------------------------------------------------------ */
/* Recording callbacks                                                 */
/* ------------------------------------------------------------------ */

typedef struct recorder {
	char deltas[4096];
	size_t deltas_len;
	int n_deltas;
	char reasoning[4096];
	size_t reasoning_len;
	char raw[4096];
	size_t raw_len;
	int n_raw;
	int cancel_after_deltas; /* nonzero: cancel once n_deltas == N */
} recorder;

static int32_t rec_delta(void *ud, const char *bytes, size_t len)
{
	recorder *rec = ud;

	memcpy(rec->deltas + rec->deltas_len, bytes, len);
	rec->deltas_len += len;
	rec->n_deltas++;
	if (rec->cancel_after_deltas != 0 &&
		rec->n_deltas >= rec->cancel_after_deltas) {
		return 1;
	}
	return 0;
}

static int32_t rec_reasoning(void *ud, const char *bytes, size_t len)
{
	recorder *rec = ud;

	memcpy(rec->reasoning + rec->reasoning_len, bytes, len);
	rec->reasoning_len += len;
	return 0;
}

static int32_t rec_raw(void *ud, const char *json, size_t len)
{
	recorder *rec = ud;

	memcpy(rec->raw + rec->raw_len, json, len);
	rec->raw_len += len;
	rec->n_raw++;
	return 0;
}

static sicha_callbacks rec_cbs(recorder *rec)
{
	sicha_callbacks cbs;

	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = rec;
	cbs.on_delta = rec_delta;
	cbs.on_reasoning = rec_reasoning;
	cbs.on_raw_chunk = rec_raw;
	return cbs;
}

/* ------------------------------------------------------------------ */
/* Non-streaming parse                                                 */
/* ------------------------------------------------------------------ */

static void check_parse_full(void)
{
	sicha_result *r = sicha_result_create(0);
	int32_t bs = -1;
	static const char body[] =
		"{\"id\":\"cmpl-1\",\"model\":\"gpt-x\",\"choices\":[{"
		"\"message\":{\"role\":\"assistant\",\"content\":\"Hello"
		" there\",\"reasoning\":\"hmm\"},\"finish_reason\":"
		"\"stop\"}],\"usage\":{\"prompt_tokens\":12,"
		"\"completion_tokens\":3,\"total_tokens\":15}}";

	T_CHECK(r != NULL);
	T_CHECK(sicha_response_parse(r, body, sizeof(body) - 1, &bs) ==
		SICHA_OK);
	T_CHECK(bs == SICHA_BODY_OK);
	{
		size_t len = 0;

		T_CHECK(strcmp(sicha_result_text(r, &len),
			"Hello there") == 0);
		T_CHECK(len == 11);
		T_CHECK(strcmp(sicha_result_reasoning(r, &len),
			"hmm") == 0);
		T_CHECK(len == 3);
	}
	T_CHECK(sicha_result_finish_reason(r) == SICHA_FINISH_STOP);
	T_CHECK(strcmp(sicha_result_finish_reason_raw(r), "stop") == 0);
	T_CHECK(sicha_result_prompt_tokens(r) == 12);
	T_CHECK(sicha_result_completion_tokens(r) == 3);
	T_CHECK(sicha_result_total_tokens(r) == 15);
	T_CHECK(strcmp(sicha_result_model(r), "gpt-x") == 0);
	sicha_result_destroy(r);
}

static void parse_expect_state(const char *name, const char *body,
	int32_t expect_state)
{
	sicha_result *r = sicha_result_create(0);
	int32_t bs = -1;
	sicha_status st = sicha_response_parse(r, body, strlen(body),
		&bs);

	t_check_impl(st == SICHA_OK && bs == expect_state, name,
		__FILE__, __LINE__);
	sicha_result_destroy(r);
}

static void check_parse_taxonomy(void)
{
	parse_expect_state("empty body", "", SICHA_BODY_EMPTY);
	parse_expect_state("not json", "<html>oops</html>",
		SICHA_BODY_MALFORMED);
	parse_expect_state("json but not object", "[1,2]",
		SICHA_BODY_MALFORMED);
	parse_expect_state("no choices", "{\"id\":\"x\"}",
		SICHA_BODY_MALFORMED);
	parse_expect_state("choices empty", "{\"choices\":[]}",
		SICHA_BODY_MALFORMED);
	parse_expect_state("choices not array", "{\"choices\":7}",
		SICHA_BODY_MALFORMED);
	parse_expect_state("choice not object", "{\"choices\":[7]}",
		SICHA_BODY_MALFORMED);
	parse_expect_state("no message", "{\"choices\":[{}]}",
		SICHA_BODY_MALFORMED);
	parse_expect_state("null content alone",
		"{\"choices\":[{\"message\":{\"content\":null}}]}",
		SICHA_BODY_MALFORMED);
	parse_expect_state("tool_calls empty array",
		"{\"choices\":[{\"message\":{\"content\":null,"
		"\"tool_calls\":[]}}]}", SICHA_BODY_MALFORMED);
	parse_expect_state("content number",
		"{\"choices\":[{\"message\":{\"content\":42}}]}",
		SICHA_BODY_MALFORMED);
	parse_expect_state("bare minimum ok",
		"{\"choices\":[{\"message\":{\"content\":\"\"}}]}",
		SICHA_BODY_OK);
}

static void check_parse_tool_calls_turn(void)
{
	sicha_result *r = sicha_result_create(0);
	int32_t bs = -1;
	static const char body[] =
		"{\"choices\":[{\"message\":{\"content\":null,"
		"\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
		"\"function\":{\"name\":\"f\",\"arguments\":\"{}\"}}]},"
		"\"finish_reason\":\"tool_calls\"}]}";

	T_CHECK(sicha_response_parse(r, body, sizeof(body) - 1, &bs) ==
		SICHA_OK);
	T_CHECK(bs == SICHA_BODY_OK);
	T_CHECK(sicha_result_finish_reason(r) == SICHA_FINISH_TOOL_CALLS);
	{
		size_t len = 99;

		T_CHECK(strcmp(sicha_result_text(r, &len), "") == 0);
		T_CHECK(len == 0);
	}
	sicha_result_destroy(r);
}

static void check_parse_finish_reasons(void)
{
	static const struct {
		const char *raw;
		sicha_finish_reason want;
	} cases[] = {
		{ "stop", SICHA_FINISH_STOP },
		{ "length", SICHA_FINISH_LENGTH },
		{ "tool_calls", SICHA_FINISH_TOOL_CALLS },
		{ "content_filter", SICHA_FINISH_CONTENT_FILTER },
		{ "eos_token", SICHA_FINISH_UNKNOWN },
		{ "STOP", SICHA_FINISH_UNKNOWN },
	};

	for (size_t i = 0; i < SICHA_COUNTOF(cases); i++) {
		sicha_result *r = sicha_result_create(0);
		char body[256];
		int32_t bs = -1;

		snprintf(body, sizeof(body),
			"{\"choices\":[{\"message\":{\"content\":\"x\"},"
			"\"finish_reason\":\"%s\"}]}", cases[i].raw);
		T_CHECK(sicha_response_parse(r, body, strlen(body), &bs) ==
			SICHA_OK);
		T_CHECK(bs == SICHA_BODY_OK);
		T_CHECK(sicha_result_finish_reason(r) == cases[i].want);
		T_CHECK(strcmp(sicha_result_finish_reason_raw(r),
			cases[i].raw) == 0);
		sicha_result_destroy(r);
	}
	/* null finish_reason stays UNKNOWN with empty raw */
	{
		sicha_result *r = sicha_result_create(0);
		int32_t bs = -1;
		static const char body[] = "{\"choices\":[{\"message\":"
			"{\"content\":\"x\"},\"finish_reason\":null}]}";

		T_CHECK(sicha_response_parse(r, body, sizeof(body) - 1,
			&bs) == SICHA_OK);
		T_CHECK(sicha_result_finish_reason(r) ==
			SICHA_FINISH_UNKNOWN);
		T_CHECK(strcmp(sicha_result_finish_reason_raw(r), "") == 0);
		sicha_result_destroy(r);
	}
}

static void check_parse_reasoning_positions(void)
{
	static const char *bodies[] = {
		/* message.reasoning */
		"{\"choices\":[{\"message\":{\"content\":\"x\","
		"\"reasoning\":\"think\"}}]}",
		/* message.reasoning_content (o-series style) */
		"{\"choices\":[{\"message\":{\"content\":\"x\","
		"\"reasoning_content\":\"think\"}}]}",
		/* choices[0].reasoning */
		"{\"choices\":[{\"reasoning\":\"think\",\"message\":"
		"{\"content\":\"x\"}}]}",
	};

	for (size_t i = 0; i < SICHA_COUNTOF(bodies); i++) {
		sicha_result *r = sicha_result_create(0);
		int32_t bs = -1;

		T_CHECK(sicha_response_parse(r, bodies[i],
			strlen(bodies[i]), &bs) == SICHA_OK);
		T_CHECK(bs == SICHA_BODY_OK);
		T_CHECK(strcmp(sicha_result_reasoning(r, NULL),
			"think") == 0);
		sicha_result_destroy(r);
	}
}

static void check_parse_partial_usage(void)
{
	sicha_result *r = sicha_result_create(0);
	int32_t bs = -1;
	static const char body[] = "{\"choices\":[{\"message\":"
		"{\"content\":\"x\"}}],\"usage\":{\"prompt_tokens\":7,"
		"\"completion_tokens\":\"three\"}}";

	T_CHECK(sicha_response_parse(r, body, sizeof(body) - 1, &bs) ==
		SICHA_OK);
	T_CHECK(bs == SICHA_BODY_OK);
	T_CHECK(sicha_result_prompt_tokens(r) == 7);
	T_CHECK(sicha_result_completion_tokens(r) == -1);
	T_CHECK(sicha_result_total_tokens(r) == -1);
	sicha_result_destroy(r);
}

static void check_parse_toobig(void)
{
	sicha_result *r = sicha_result_create(8); /* tiny content cap */
	int32_t bs = -1;
	static const char body[] = "{\"choices\":[{\"message\":"
		"{\"content\":\"way more than eight bytes\"}}]}";

	T_CHECK(sicha_response_parse(r, body, sizeof(body) - 1, &bs) ==
		SICHA_OK);
	T_CHECK(bs == SICHA_BODY_TOOBIG);
	sicha_result_destroy(r);
}

/* ------------------------------------------------------------------ */
/* Streaming assembly                                                  */
/* ------------------------------------------------------------------ */

/* Feed one JSON event; return the on_event rc. */
static int32_t ev(sicha_stream_ctx *ctx, const char *json)
{
	return sicha_stream_on_event(ctx, json, strlen(json));
}

static void check_stream_happy(void)
{
	sicha_result *r = sicha_result_create(0);
	recorder rec;
	sicha_callbacks cbs;
	sicha_stream_ctx ctx;

	memset(&rec, 0, sizeof(rec));
	cbs = rec_cbs(&rec);
	sicha_stream_ctx_init(&ctx, r, &cbs, 1);

	T_CHECK(ev(&ctx, "{\"model\":\"m-1\",\"choices\":[{\"delta\":"
		"{\"role\":\"assistant\",\"content\":\"Hel\"}}]}") == 0);
	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":{\"content\":"
		"\"lo\"}}]}") == 0);
	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":{},"
		"\"finish_reason\":\"stop\"}]}") == 0);
	T_CHECK(ev(&ctx, "{\"choices\":[],\"usage\":"
		"{\"prompt_tokens\":10,\"completion_tokens\":2,"
		"\"total_tokens\":12}}") == 0);
	T_CHECK(sicha_stream_on_event(&ctx, "[DONE]", 6) ==
		SICHA_STREAM_STOP);

	T_CHECK(ctx.done == 1);
	T_CHECK(ctx.delivered == 1);
	T_CHECK(ctx.err == SICHA_OK);
	T_CHECK(rec.n_deltas == 2);
	T_CHECK(rec.deltas_len == 5 &&
		memcmp(rec.deltas, "Hello", 5) == 0);
	T_CHECK(strcmp(sicha_result_text(r, NULL), "Hello") == 0);
	T_CHECK(sicha_result_finish_reason(r) == SICHA_FINISH_STOP);
	T_CHECK(sicha_result_prompt_tokens(r) == 10);
	T_CHECK(sicha_result_total_tokens(r) == 12);
	T_CHECK(strcmp(sicha_result_model(r), "m-1") == 0);
	T_CHECK(rec.n_raw == 0);

	sicha_stream_ctx_free(&ctx);
	sicha_result_destroy(r);
}

static void check_stream_reasoning_interleaved(void)
{
	sicha_result *r = sicha_result_create(0);
	recorder rec;
	sicha_callbacks cbs;
	sicha_stream_ctx ctx;

	memset(&rec, 0, sizeof(rec));
	cbs = rec_cbs(&rec);
	sicha_stream_ctx_init(&ctx, r, &cbs, 1);

	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":{\"reasoning\":"
		"\"let me \"}}]}") == 0);
	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":"
		"{\"reasoning_content\":\"think\"}}]}") == 0);
	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":{\"content\":\"Hi\","
		"\"reasoning\":\"!\"}}]}") == 0);

	T_CHECK(rec.reasoning_len == 13 &&
		memcmp(rec.reasoning, "let me think!", 13) == 0);
	T_CHECK(strcmp(sicha_result_reasoning(r, NULL),
		"let me think!") == 0);
	T_CHECK(strcmp(sicha_result_text(r, NULL), "Hi") == 0);

	sicha_stream_ctx_free(&ctx);
	sicha_result_destroy(r);
}

static void check_stream_raw_chunks(void)
{
	sicha_result *r = sicha_result_create(0);
	recorder rec;
	sicha_callbacks cbs;
	sicha_stream_ctx ctx;

	memset(&rec, 0, sizeof(rec));
	cbs = rec_cbs(&rec);
	sicha_stream_ctx_init(&ctx, r, &cbs, 1);

	/* malformed json: forwarded, not fatal */
	T_CHECK(ev(&ctx, "not json {{{") == 0);
	T_CHECK(rec.n_raw == 1);
	/* interleaved error event: forwarded */
	T_CHECK(ev(&ctx, "{\"error\":{\"message\":\"provider died\","
		"\"code\":502}}") == 0);
	T_CHECK(rec.n_raw == 2);
	/* tool-call delta: forwarded AND stream continues */
	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":{\"tool_calls\":"
		"[{\"index\":0,\"function\":{\"name\":\"f\"}}]}}]}") == 0);
	T_CHECK(rec.n_raw == 3);
	/* the stream is still healthy afterwards */
	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":{\"content\":"
		"\"ok\"}}]}") == 0);
	T_CHECK(strcmp(sicha_result_text(r, NULL), "ok") == 0);
	/* last_raw kept the most recent forwarded payload */
	T_CHECK(ctx.last_raw.len > 0);
	T_CHECK(strstr(ctx.last_raw.data, "tool_calls") != NULL);

	sicha_stream_ctx_free(&ctx);
	sicha_result_destroy(r);
}

static void check_stream_no_accumulate(void)
{
	sicha_result *r = sicha_result_create(0);
	recorder rec;
	sicha_callbacks cbs;
	sicha_stream_ctx ctx;

	memset(&rec, 0, sizeof(rec));
	cbs = rec_cbs(&rec);
	sicha_stream_ctx_init(&ctx, r, &cbs, 0);

	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":{\"content\":"
		"\"big\"}}]}") == 0);
	T_CHECK(rec.deltas_len == 3);
	T_CHECK(strcmp(sicha_result_text(r, NULL), "") == 0);
	T_CHECK(ctx.delivered == 1);

	sicha_stream_ctx_free(&ctx);
	sicha_result_destroy(r);
}

static void check_stream_without_callbacks(void)
{
	/* no callbacks at all: pure accumulation, delivered stays 0 (a
	 * retry cannot corrupt what the caller never saw) */
	sicha_result *r = sicha_result_create(0);
	sicha_stream_ctx ctx;

	sicha_stream_ctx_init(&ctx, r, NULL, 1);
	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":{\"content\":"
		"\"quiet\"}}]}") == 0);
	T_CHECK(ctx.delivered == 0);
	T_CHECK(strcmp(sicha_result_text(r, NULL), "quiet") == 0);
	sicha_stream_ctx_free(&ctx);
	sicha_result_destroy(r);
}

static void check_stream_cancel_from_callback(void)
{
	sicha_result *r = sicha_result_create(0);
	recorder rec;
	sicha_callbacks cbs;
	sicha_stream_ctx ctx;

	memset(&rec, 0, sizeof(rec));
	rec.cancel_after_deltas = 2;
	cbs = rec_cbs(&rec);
	sicha_stream_ctx_init(&ctx, r, &cbs, 1);

	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":{\"content\":"
		"\"a\"}}]}") == 0);
	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":{\"content\":"
		"\"b\"}}]}") == SICHA_STREAM_CANCEL);
	T_CHECK(ctx.done == 0);
	T_CHECK(ctx.delivered == 1);

	sicha_stream_ctx_free(&ctx);
	sicha_result_destroy(r);
}

static void check_stream_content_cap(void)
{
	sicha_result *r = sicha_result_create(4);
	sicha_stream_ctx ctx;

	sicha_stream_ctx_init(&ctx, r, NULL, 1);
	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":{\"content\":"
		"\"12345678\"}}]}") == -1);
	T_CHECK(ctx.err == SICHA_INT_E_TOOBIG);
	sicha_stream_ctx_free(&ctx);
	sicha_result_destroy(r);
}

static void check_stream_embedded_nul_delta(void)
{
	sicha_result *r = sicha_result_create(0);
	sicha_stream_ctx ctx;

	sicha_stream_ctx_init(&ctx, r, NULL, 1);
	T_CHECK(ev(&ctx, "{\"choices\":[{\"delta\":{\"content\":"
		"\"a\\u0000b\"}}]}") == 0);
	{
		size_t len = 0;
		const char *text = sicha_result_text(r, &len);

		T_CHECK(len == 3);
		T_CHECK(memcmp(text, "a\0b", 3) == 0);
		T_CHECK(text[len] == '\0');
	}
	sicha_stream_ctx_free(&ctx);
	sicha_result_destroy(r);
}

/* ------------------------------------------------------------------ */
/* Result object semantics                                             */
/* ------------------------------------------------------------------ */

static void check_attempt_records(void)
{
	sicha_result *r = sicha_result_create(0);
	char model_buf[16];
	char msg_buf[32];
	sicha_attempt a;

	memset(&a, 0, sizeof(a));
	a.struct_size = (uint32_t)sizeof(a);
	a.attempt = 0;
	a.backend = 1;
	a.try_of_backend = 2;
	snprintf(model_buf, sizeof(model_buf), "model-A");
	snprintf(msg_buf, sizeof(msg_buf), "http 503");
	a.model = model_buf;
	a.message = msg_buf;
	a.error_class = SICHA_CLASS_RETRY_SAME;
	a.http_status = 503;
	a.transport_status = SICHA_T_OK;
	a.latency_ms = 42;
	a.prompt_tokens = -1;
	a.completion_tokens = -1;
	a.total_tokens = -1;
	a.raw_body_excerpt = "{\"error\":\"overloaded\"}";
	a.raw_body_excerpt_len = strlen(a.raw_body_excerpt);

	T_CHECK(sicha_result_add_attempt(r, &a) == SICHA_OK);
	/* deep copy: clobber the sources */
	memset(model_buf, 'X', sizeof(model_buf) - 1);
	memset(msg_buf, 'Y', sizeof(msg_buf) - 1);

	T_CHECK(sicha_result_attempt_count(r) == 1);
	{
		const sicha_attempt *got = sicha_result_attempt(r, 0);

		T_CHECK(got != NULL);
		T_CHECK(got != NULL &&
			strcmp(got->model, "model-A") == 0);
		T_CHECK(got != NULL &&
			strcmp(got->message, "http 503") == 0);
		T_CHECK(got != NULL && strcmp(got->raw_body_excerpt,
			"{\"error\":\"overloaded\"}") == 0);
		T_CHECK(got != NULL && got->raw_body_excerpt_len ==
			strlen("{\"error\":\"overloaded\"}"));
		T_CHECK(got != NULL && got->backend == 1);
		T_CHECK(got != NULL && got->latency_ms == 42);
	}
	T_CHECK(sicha_result_attempt(r, 1) == NULL);
	T_CHECK(sicha_result_attempt(r, 0xFFFFFFFF) == NULL);

	/* growth across the initial capacity */
	for (int i = 0; i < 20; i++) {
		a.model = NULL; /* NULL string fields tolerated */
		a.message = NULL;
		a.raw_body_excerpt = NULL;
		a.raw_body_excerpt_len = 0;
		a.attempt = (uint32_t)i + 1;
		T_CHECK(sicha_result_add_attempt(r, &a) == SICHA_OK);
	}
	T_CHECK(sicha_result_attempt_count(r) == 21);
	{
		const sicha_attempt *got = sicha_result_attempt(r, 20);

		T_CHECK(got != NULL && got->attempt == 20);
		T_CHECK(got != NULL && strcmp(got->model, "") == 0);
	}
	/* excerpt cap enforced on copy */
	{
		char big[SICHA_EXCERPT_MAX + 100];

		memset(big, 'z', sizeof(big));
		a.raw_body_excerpt = big;
		a.raw_body_excerpt_len = sizeof(big);
		T_CHECK(sicha_result_add_attempt(r, &a) == SICHA_OK);
		T_CHECK(sicha_result_attempt(r, 21)->raw_body_excerpt_len ==
			SICHA_EXCERPT_MAX);
	}
	sicha_result_destroy(r);
}

static void check_reset_content(void)
{
	sicha_result *r = sicha_result_create(0);
	int32_t bs = -1;
	static const char body[] =
		"{\"model\":\"m\",\"choices\":[{\"message\":{\"content\":"
		"\"text\",\"reasoning\":\"why\"},\"finish_reason\":"
		"\"stop\"}],\"usage\":{\"prompt_tokens\":1,"
		"\"completion_tokens\":2,\"total_tokens\":3}}";
	sicha_attempt a;

	memset(&a, 0, sizeof(a));
	T_CHECK(sicha_response_parse(r, body, sizeof(body) - 1, &bs) ==
		SICHA_OK);
	T_CHECK(sicha_result_add_attempt(r, &a) == SICHA_OK);

	sicha_result_reset_content(r);
	T_CHECK(strcmp(sicha_result_text(r, NULL), "") == 0);
	T_CHECK(strcmp(sicha_result_reasoning(r, NULL), "") == 0);
	T_CHECK(sicha_result_finish_reason(r) == SICHA_FINISH_UNKNOWN);
	T_CHECK(strcmp(sicha_result_finish_reason_raw(r), "") == 0);
	T_CHECK(strcmp(sicha_result_model(r), "") == 0);
	T_CHECK(sicha_result_prompt_tokens(r) == -1);
	/* attempt records persist across resets */
	T_CHECK(sicha_result_attempt_count(r) == 1);
	sicha_result_destroy(r);
}

static void check_null_getters(void)
{
	size_t len = 77;

	T_CHECK(sicha_result_status(NULL) == SICHA_E_INVALID_ARG);
	T_CHECK(strcmp(sicha_result_text(NULL, &len), "") == 0);
	T_CHECK(len == 0);
	T_CHECK(strcmp(sicha_result_reasoning(NULL, NULL), "") == 0);
	T_CHECK(sicha_result_finish_reason(NULL) == SICHA_FINISH_UNKNOWN);
	T_CHECK(strcmp(sicha_result_finish_reason_raw(NULL), "") == 0);
	T_CHECK(sicha_result_prompt_tokens(NULL) == -1);
	T_CHECK(strcmp(sicha_result_model(NULL), "") == 0);
	T_CHECK(sicha_result_backend(NULL) == -1);
	T_CHECK(strcmp(sicha_result_raw_body(NULL, &len), "") == 0);
	T_CHECK(sicha_result_attempt_count(NULL) == 0);
	T_CHECK(sicha_result_attempt(NULL, 0) == NULL);
	sicha_result_destroy(NULL); /* NULL-safe */
}

int main(void)
{
	check_parse_full();
	check_parse_taxonomy();
	check_parse_tool_calls_turn();
	check_parse_finish_reasons();
	check_parse_reasoning_positions();
	check_parse_partial_usage();
	check_parse_toobig();
	check_stream_happy();
	check_stream_reasoning_interleaved();
	check_stream_raw_chunks();
	check_stream_no_accumulate();
	check_stream_without_callbacks();
	check_stream_cancel_from_callback();
	check_stream_content_cap();
	check_stream_embedded_nul_delta();
	check_attempt_records();
	check_reset_content();
	check_null_getters();
	return t_done("test_response");
}
