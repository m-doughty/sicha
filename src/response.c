/* The result object (public getters + internal assembly) and the two
 * response-assembly paths: non-streaming JSON bodies and streaming
 * SSE chat-completion chunks.
 *
 * Wire tolerance rules (ported from LLM::Chat):
 *   - a 2xx body that cannot be understood NEVER hard-errors — it is
 *     reported as a SICHA_BODY_* state and classified (advance);
 *   - malformed or unmodeled SSE payloads (tool-call deltas,
 *     interleaved {"error":...} fragments, provider oddities) are
 *     forwarded to on_raw_chunk and skipped, never fatal;
 *   - "choices":[] events are legal (usage-only final chunks);
 *   - null content WITH tool_calls is a successful tool-call turn
 *     (the verbatim body is retained so nothing is dropped). */

#include <stdlib.h>
#include <string.h>

#include "sicha_internal.h"
#include "yyjson.h"

static const char EMPTY[] = "";

/* ------------------------------------------------------------------ */
/* Result lifecycle                                                    */
/* ------------------------------------------------------------------ */

sicha_result *sicha_result_create(size_t max_response_bytes)
{
	sicha_result *r = calloc(1, sizeof(*r));

	if (r == NULL) {
		return NULL;
	}
	sicha_buf_init(&r->text, max_response_bytes);
	sicha_buf_init(&r->reasoning, max_response_bytes);
	r->status = SICHA_OK;
	r->finish = SICHA_FINISH_UNKNOWN;
	r->prompt_tokens = -1;
	r->completion_tokens = -1;
	r->total_tokens = -1;
	r->backend = -1;
	return r;
}

void sicha_result_destroy(sicha_result *r)
{
	if (r == NULL) {
		return;
	}
	sicha_buf_free(&r->text);
	sicha_buf_free(&r->reasoning);
	free(r->finish_raw);
	free(r->model);
	free(r->raw_body);
	for (uint32_t i = 0; i < r->n_attempts; i++) {
		free((char *)(uintptr_t)r->attempts[i].model);
		free((char *)(uintptr_t)r->attempts[i].message);
		free((char *)(uintptr_t)r->attempts[i].raw_body_excerpt);
	}
	free(r->attempts);
	free(r);
}

static char *dup_bytes(const char *s, size_t len)
{
	char *p = malloc(len + 1);

	if (p != NULL) {
		memcpy(p, s, len);
		p[len] = '\0';
	}
	return p;
}

sicha_status sicha_result_add_attempt(sicha_result *r,
	const sicha_attempt *a)
{
	sicha_attempt copy = *a;

	if (r->n_attempts == r->cap_attempts) {
		uint32_t cap = r->cap_attempts == 0 ? 4 :
			r->cap_attempts * 2;
		sicha_attempt *p = realloc(r->attempts,
			(size_t)cap * sizeof(*p));

		if (p == NULL) {
			return SICHA_E_NOMEM;
		}
		r->attempts = p;
		r->cap_attempts = cap;
	}
	copy.struct_size = (uint32_t)sizeof(copy);
	copy.model = dup_bytes(a->model != NULL ? a->model : "",
		a->model != NULL ? strlen(a->model) : 0);
	copy.message = dup_bytes(a->message != NULL ? a->message : "",
		a->message != NULL ? strlen(a->message) : 0);
	{
		size_t elen = a->raw_body_excerpt != NULL ?
			a->raw_body_excerpt_len : 0;

		if (elen > SICHA_EXCERPT_MAX) {
			elen = SICHA_EXCERPT_MAX;
		}
		copy.raw_body_excerpt = dup_bytes(
			a->raw_body_excerpt != NULL ? a->raw_body_excerpt :
			"", elen);
		copy.raw_body_excerpt_len = elen;
	}
	if (copy.model == NULL || copy.message == NULL ||
		copy.raw_body_excerpt == NULL) {
		free((char *)(uintptr_t)copy.model);
		free((char *)(uintptr_t)copy.message);
		free((char *)(uintptr_t)copy.raw_body_excerpt);
		return SICHA_E_NOMEM;
	}
	r->attempts[r->n_attempts++] = copy;
	return SICHA_OK;
}

void sicha_result_reset_content(sicha_result *r)
{
	sicha_buf_reset(&r->text);
	sicha_buf_reset(&r->reasoning);
	free(r->finish_raw);
	r->finish_raw = NULL;
	r->finish = SICHA_FINISH_UNKNOWN;
	free(r->model);
	r->model = NULL;
	free(r->raw_body);
	r->raw_body = NULL;
	r->raw_body_len = 0;
	r->prompt_tokens = -1;
	r->completion_tokens = -1;
	r->total_tokens = -1;
}

sicha_status sicha_result_set_model(sicha_result *r, const char *s,
	size_t len)
{
	char *p = dup_bytes(s, len);

	if (p == NULL) {
		return SICHA_E_NOMEM;
	}
	free(r->model);
	r->model = p;
	return SICHA_OK;
}

static sicha_finish_reason map_finish(const char *s, size_t len)
{
	if (len == 4 && memcmp(s, "stop", 4) == 0) {
		return SICHA_FINISH_STOP;
	}
	if (len == 6 && memcmp(s, "length", 6) == 0) {
		return SICHA_FINISH_LENGTH;
	}
	if (len == 10 && memcmp(s, "tool_calls", 10) == 0) {
		return SICHA_FINISH_TOOL_CALLS;
	}
	if (len == 14 && memcmp(s, "content_filter", 14) == 0) {
		return SICHA_FINISH_CONTENT_FILTER;
	}
	return SICHA_FINISH_UNKNOWN;
}

sicha_status sicha_result_set_finish(sicha_result *r, const char *raw,
	size_t len)
{
	char *p = dup_bytes(raw, len);

	if (p == NULL) {
		return SICHA_E_NOMEM;
	}
	free(r->finish_raw);
	r->finish_raw = p;
	r->finish = map_finish(raw, len);
	return SICHA_OK;
}

/* ------------------------------------------------------------------ */
/* Public getters (inert on NULL, never return NULL strings)           */
/* ------------------------------------------------------------------ */

sicha_status sicha_result_status(const sicha_result *r)
{
	return r == NULL ? SICHA_E_INVALID_ARG : r->status;
}

const char *sicha_result_text(const sicha_result *r, size_t *len)
{
	const char *s = r != NULL && r->text.data != NULL ? r->text.data :
		EMPTY;

	if (len != NULL) {
		*len = r != NULL ? r->text.len : 0;
	}
	return s;
}

const char *sicha_result_reasoning(const sicha_result *r, size_t *len)
{
	const char *s = r != NULL && r->reasoning.data != NULL ?
		r->reasoning.data : EMPTY;

	if (len != NULL) {
		*len = r != NULL ? r->reasoning.len : 0;
	}
	return s;
}

sicha_finish_reason sicha_result_finish_reason(const sicha_result *r)
{
	return r == NULL ? SICHA_FINISH_UNKNOWN : r->finish;
}

const char *sicha_result_finish_reason_raw(const sicha_result *r)
{
	return r != NULL && r->finish_raw != NULL ? r->finish_raw : EMPTY;
}

int64_t sicha_result_prompt_tokens(const sicha_result *r)
{
	return r == NULL ? -1 : r->prompt_tokens;
}

int64_t sicha_result_completion_tokens(const sicha_result *r)
{
	return r == NULL ? -1 : r->completion_tokens;
}

int64_t sicha_result_total_tokens(const sicha_result *r)
{
	return r == NULL ? -1 : r->total_tokens;
}

const char *sicha_result_model(const sicha_result *r)
{
	return r != NULL && r->model != NULL ? r->model : EMPTY;
}

int32_t sicha_result_backend(const sicha_result *r)
{
	return r == NULL ? -1 : r->backend;
}

const char *sicha_result_raw_body(const sicha_result *r, size_t *len)
{
	const char *s = r != NULL && r->raw_body != NULL ? r->raw_body :
		EMPTY;

	if (len != NULL) {
		*len = r != NULL ? r->raw_body_len : 0;
	}
	return s;
}

uint32_t sicha_result_attempt_count(const sicha_result *r)
{
	return r == NULL ? 0 : r->n_attempts;
}

const sicha_attempt *sicha_result_attempt(const sicha_result *r,
	uint32_t i)
{
	if (r == NULL || i >= r->n_attempts) {
		return NULL;
	}
	return &r->attempts[i];
}

/* ------------------------------------------------------------------ */
/* Shared JSON lifting                                                 */
/* ------------------------------------------------------------------ */

static int64_t lift_i64(yyjson_val *obj, const char *key)
{
	yyjson_val *v = yyjson_obj_get(obj, key);

	if (v == NULL || !yyjson_is_num(v)) {
		return -1;
	}
	return yyjson_get_sint(v);
}

static sicha_status lift_usage(sicha_result *r, yyjson_val *root)
{
	yyjson_val *usage = yyjson_obj_get(root, "usage");

	if (usage == NULL || !yyjson_is_obj(usage)) {
		return SICHA_OK;
	}
	{
		int64_t p = lift_i64(usage, "prompt_tokens");
		int64_t c = lift_i64(usage, "completion_tokens");
		int64_t t = lift_i64(usage, "total_tokens");

		if (p >= 0) {
			r->prompt_tokens = p;
		}
		if (c >= 0) {
			r->completion_tokens = c;
		}
		if (t >= 0) {
			r->total_tokens = t;
		}
	}
	return SICHA_OK;
}

static sicha_status lift_model(sicha_result *r, yyjson_val *root)
{
	yyjson_val *m = yyjson_obj_get(root, "model");

	if (r->model == NULL && m != NULL && yyjson_is_str(m)) {
		return sicha_result_set_model(r, yyjson_get_str(m),
			yyjson_get_len(m));
	}
	return SICHA_OK;
}

/* choices[0].message.reasoning / .reasoning_content /
 * choices[0].reasoning — first hit wins. */
static yyjson_val *find_reasoning(yyjson_val *choice0, yyjson_val *msg)
{
	yyjson_val *v;

	if (msg != NULL) {
		v = yyjson_obj_get(msg, "reasoning");
		if (v != NULL && yyjson_is_str(v)) {
			return v;
		}
		v = yyjson_obj_get(msg, "reasoning_content");
		if (v != NULL && yyjson_is_str(v)) {
			return v;
		}
	}
	v = yyjson_obj_get(choice0, "reasoning");
	if (v != NULL && yyjson_is_str(v)) {
		return v;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Non-streaming parse                                                 */
/* ------------------------------------------------------------------ */

sicha_status sicha_response_parse(sicha_result *r, const char *body,
	size_t len, int32_t *body_state)
{
	yyjson_doc *doc;
	yyjson_val *root;
	yyjson_val *choices;
	yyjson_val *choice0;
	yyjson_val *msg;
	yyjson_val *content;
	sicha_status st = SICHA_OK;

	*body_state = SICHA_BODY_MALFORMED;
	if (len == 0) {
		*body_state = SICHA_BODY_EMPTY;
		return SICHA_OK;
	}
	doc = yyjson_read(body, len, 0);
	if (doc == NULL) {
		return SICHA_OK; /* malformed */
	}
	root = yyjson_doc_get_root(doc);
	if (!yyjson_is_obj(root)) {
		goto out;
	}
	choices = yyjson_obj_get(root, "choices");
	if (choices == NULL || !yyjson_is_arr(choices) ||
		yyjson_arr_size(choices) == 0) {
		goto out;
	}
	choice0 = yyjson_arr_get_first(choices);
	if (!yyjson_is_obj(choice0)) {
		goto out;
	}
	msg = yyjson_obj_get(choice0, "message");
	if (msg != NULL && !yyjson_is_obj(msg)) {
		msg = NULL;
	}
	content = msg != NULL ? yyjson_obj_get(msg, "content") : NULL;

	if (content != NULL && yyjson_is_str(content)) {
		sicha_buf_append(&r->text, yyjson_get_str(content),
			yyjson_get_len(content));
		st = sicha_buf_status(&r->text);
		if (st == SICHA_INT_E_TOOBIG) {
			*body_state = SICHA_BODY_TOOBIG;
			st = SICHA_OK;
			goto out;
		}
		if (st != SICHA_OK) {
			goto out;
		}
	} else {
		/* null / absent content: a tool-call turn is success
		 * (the raw body carries the calls); otherwise the
		 * response is unusable */
		yyjson_val *tc = msg != NULL ?
			yyjson_obj_get(msg, "tool_calls") : NULL;

		if (tc == NULL || !yyjson_is_arr(tc) ||
			yyjson_arr_size(tc) == 0) {
			goto out;
		}
	}

	{
		yyjson_val *reas = find_reasoning(choice0, msg);

		if (reas != NULL) {
			sicha_buf_append(&r->reasoning,
				yyjson_get_str(reas),
				yyjson_get_len(reas));
			st = sicha_buf_status(&r->reasoning);
			if (st == SICHA_INT_E_TOOBIG) {
				*body_state = SICHA_BODY_TOOBIG;
				st = SICHA_OK;
				goto out;
			}
			if (st != SICHA_OK) {
				goto out;
			}
		}
	}
	{
		yyjson_val *fin = yyjson_obj_get(choice0, "finish_reason");

		if (fin != NULL && yyjson_is_str(fin)) {
			st = sicha_result_set_finish(r,
				yyjson_get_str(fin), yyjson_get_len(fin));
			if (st != SICHA_OK) {
				goto out;
			}
		}
	}
	st = lift_usage(r, root);
	if (st == SICHA_OK) {
		st = lift_model(r, root);
	}
	if (st == SICHA_OK) {
		*body_state = SICHA_BODY_OK;
	}
out:
	yyjson_doc_free(doc);
	return st;
}

/* ------------------------------------------------------------------ */
/* Streaming assembly                                                  */
/* ------------------------------------------------------------------ */

void sicha_stream_ctx_init(sicha_stream_ctx *ctx, sicha_result *r,
	const sicha_callbacks *cbs, int accumulate)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->r = r;
	ctx->cbs = cbs;
	ctx->accumulate = accumulate;
	sicha_buf_init(&ctx->last_raw, SICHA_EXCERPT_MAX);
}

void sicha_stream_ctx_free(sicha_stream_ctx *ctx)
{
	sicha_buf_free(&ctx->last_raw);
}

/* Forward an unmodeled / unparseable payload to on_raw_chunk and
 * remember a capped copy for attempt records. */
static int32_t raw_chunk(sicha_stream_ctx *ctx, const char *data,
	size_t len)
{
	sicha_buf_reset(&ctx->last_raw);
	sicha_buf_append(&ctx->last_raw, data,
		len > SICHA_EXCERPT_MAX ? SICHA_EXCERPT_MAX : len);
	if (sicha_buf_status(&ctx->last_raw) == SICHA_E_NOMEM) {
		ctx->err = SICHA_E_NOMEM;
		return -1;
	}
	if (ctx->cbs != NULL && ctx->cbs->on_raw_chunk != NULL &&
		ctx->cbs->on_raw_chunk(ctx->cbs->ud, data, len) != 0) {
		return SICHA_STREAM_CANCEL;
	}
	return 0;
}

static int32_t deliver(sicha_stream_ctx *ctx, sicha_buf *accum,
	int32_t (*cb)(void *, const char *, size_t), void *ud,
	const char *bytes, size_t len)
{
	if (len == 0) {
		return 0;
	}
	if (ctx->accumulate) {
		sicha_buf_append(accum, bytes, len);
		if (sicha_buf_status(accum) == SICHA_E_NOMEM) {
			ctx->err = SICHA_E_NOMEM;
			return -1;
		}
		if (sicha_buf_status(accum) == SICHA_INT_E_TOOBIG) {
			ctx->err = SICHA_INT_E_TOOBIG;
			return -1;
		}
	}
	if (cb != NULL) {
		ctx->delivered = 1;
		if (cb(ud, bytes, len) != 0) {
			return SICHA_STREAM_CANCEL;
		}
	}
	return 0;
}

int32_t sicha_stream_on_event(void *ud, const char *data, size_t len)
{
	sicha_stream_ctx *ctx = ud;
	yyjson_doc *doc;
	yyjson_val *root;
	yyjson_val *choices;
	int32_t rc = 0;

	if (len == 6 && memcmp(data, "[DONE]", 6) == 0) {
		ctx->done = 1;
		return SICHA_STREAM_STOP;
	}
	doc = yyjson_read(data, len, 0);
	if (doc == NULL) {
		return raw_chunk(ctx, data, len);
	}
	root = yyjson_doc_get_root(doc);
	if (!yyjson_is_obj(root) ||
		yyjson_obj_get(root, "error") != NULL) {
		rc = raw_chunk(ctx, data, len);
		goto out;
	}
	ctx->saw_event = 1;

	choices = yyjson_obj_get(root, "choices");
	if (choices != NULL && yyjson_is_arr(choices) &&
		yyjson_arr_size(choices) > 0) {
		yyjson_val *choice0 = yyjson_arr_get_first(choices);
		yyjson_val *delta;

		if (!yyjson_is_obj(choice0)) {
			rc = raw_chunk(ctx, data, len);
			goto out;
		}
		delta = yyjson_obj_get(choice0, "delta");
		if (delta != NULL && yyjson_is_obj(delta)) {
			yyjson_val *content = yyjson_obj_get(delta,
				"content");
			yyjson_val *reas = yyjson_obj_get(delta,
				"reasoning");
			yyjson_val *tc = yyjson_obj_get(delta,
				"tool_calls");

			if (reas == NULL || !yyjson_is_str(reas)) {
				reas = yyjson_obj_get(delta,
					"reasoning_content");
			}
			if (content != NULL && yyjson_is_str(content)) {
				rc = deliver(ctx, &ctx->r->text,
					ctx->cbs != NULL ?
						ctx->cbs->on_delta : NULL,
					ctx->cbs != NULL ? ctx->cbs->ud :
						NULL,
					yyjson_get_str(content),
					yyjson_get_len(content));
				if (rc != 0) {
					goto out;
				}
			}
			if (reas != NULL && yyjson_is_str(reas)) {
				rc = deliver(ctx, &ctx->r->reasoning,
					ctx->cbs != NULL ?
						ctx->cbs->on_reasoning :
						NULL,
					ctx->cbs != NULL ? ctx->cbs->ud :
						NULL,
					yyjson_get_str(reas),
					yyjson_get_len(reas));
				if (rc != 0) {
					goto out;
				}
			}
			if (tc != NULL) {
				/* unmodeled in v0.1: surfaced, skipped */
				rc = raw_chunk(ctx, data, len);
				if (rc != 0) {
					goto out;
				}
			}
		}
		{
			yyjson_val *fin = yyjson_obj_get(choice0,
				"finish_reason");

			if (fin != NULL && yyjson_is_str(fin)) {
				if (sicha_result_set_finish(ctx->r,
					yyjson_get_str(fin),
					yyjson_get_len(fin)) != SICHA_OK) {
					ctx->err = SICHA_E_NOMEM;
					rc = -1;
					goto out;
				}
			}
		}
	}

	if (lift_usage(ctx->r, root) != SICHA_OK ||
		lift_model(ctx->r, root) != SICHA_OK) {
		ctx->err = SICHA_E_NOMEM;
		rc = -1;
	}
out:
	yyjson_doc_free(doc);
	return rc;
}
