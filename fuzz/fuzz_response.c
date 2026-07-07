/* libFuzzer harness for response assembly (white-box).  Mode byte
 * selects the path:
 *   even  — non-streaming: the payload is parsed as a chat-completion
 *           body;
 *   odd   — streaming: the payload is fed through the SSE parser into
 *           the stream assembler with permissive callbacks.
 * Oracles: no crash on any input; every getter stays consistent
 * (text/reasoning NUL-terminated at the reported length, enums in
 * range, body_state in range); destroy is always clean (leaks caught
 * by ASan).
 *
 * Build with -DSICHA_FUZZ=ON (Clang only); run e.g.
 *   ./fuzz_response -max_total_time=60 corpus/
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sicha_internal.h"

#define FUZZ_ASSERT(cond) \
	do { \
		if (!(cond)) { \
			abort(); \
		} \
	} while (0)

static int32_t sink_bytes(void *ud, const char *bytes, size_t len)
{
	uint64_t *fnv = ud;

	for (size_t i = 0; i < len; i++) {
		*fnv = (*fnv ^ (uint8_t)bytes[i]) * 0x100000001B3ull;
	}
	return 0;
}

static void check_result_invariants(const sicha_result *r)
{
	size_t len = 0;
	const char *s;

	s = sicha_result_text(r, &len);
	FUZZ_ASSERT(s != NULL);
	FUZZ_ASSERT(s[len] == '\0');
	s = sicha_result_reasoning(r, &len);
	FUZZ_ASSERT(s != NULL);
	FUZZ_ASSERT(s[len] == '\0');
	FUZZ_ASSERT(sicha_result_finish_reason_raw(r) != NULL);
	{
		sicha_finish_reason f = sicha_result_finish_reason(r);

		FUZZ_ASSERT(f >= SICHA_FINISH_UNKNOWN &&
			f <= SICHA_FINISH_CONTENT_FILTER);
	}
	FUZZ_ASSERT(sicha_result_model(r) != NULL);
	FUZZ_ASSERT(sicha_result_prompt_tokens(r) >= -1);
	FUZZ_ASSERT(sicha_result_completion_tokens(r) >= -1);
	FUZZ_ASSERT(sicha_result_total_tokens(r) >= -1);
	for (uint32_t i = 0; i < sicha_result_attempt_count(r); i++) {
		const sicha_attempt *a = sicha_result_attempt(r, i);

		FUZZ_ASSERT(a != NULL && a->model != NULL &&
			a->message != NULL && a->raw_body_excerpt != NULL);
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint8_t mode;
	size_t cap;
	const char *payload;
	size_t payload_len;
	sicha_result *r;

	if (size < 2) {
		return 0;
	}
	mode = data[0];
	cap = mode % 8 == 0 ? 1u + (size_t)data[1] * 8u : 0;
	payload = (const char *)data + 2;
	payload_len = size - 2;

	r = sicha_result_create(cap);
	FUZZ_ASSERT(r != NULL);

	if (mode % 2 == 0) {
		int32_t bs = -1;
		sicha_status st = sicha_response_parse(r, payload,
			payload_len, &bs);

		FUZZ_ASSERT(st == SICHA_OK || st == SICHA_E_NOMEM);
		FUZZ_ASSERT(bs == SICHA_BODY_OK || bs == SICHA_BODY_EMPTY ||
			bs == SICHA_BODY_MALFORMED ||
			bs == SICHA_BODY_TOOBIG);
	} else {
		uint64_t fnv = 0xcbf29ce484222325ull;
		sicha_callbacks cbs;
		sicha_stream_ctx ctx;
		sicha_sse sse;
		int32_t rc;

		memset(&cbs, 0, sizeof(cbs));
		cbs.struct_size = (uint32_t)sizeof(cbs);
		cbs.ud = &fnv;
		cbs.on_delta = mode % 4 == 1 ? sink_bytes : NULL;
		cbs.on_reasoning = mode % 4 == 1 ? sink_bytes : NULL;
		cbs.on_raw_chunk = mode % 8 < 4 ? sink_bytes : NULL;
		sicha_stream_ctx_init(&ctx, r, mode % 16 == 15 ? NULL :
			&cbs, mode % 4 != 3);
		sicha_sse_init(&sse, cap);

		rc = sicha_sse_feed(&sse, payload, payload_len,
			sicha_stream_on_event, &ctx);
		if (rc == 0) {
			rc = sicha_sse_finish(&sse, sicha_stream_on_event,
				&ctx);
		}
		FUZZ_ASSERT(rc == 0 || rc == -1 || rc == SICHA_STREAM_STOP ||
			rc == SICHA_STREAM_CANCEL);
		if (rc == -1) {
			FUZZ_ASSERT(ctx.err != SICHA_OK ||
				sicha_sse_status(&sse) != SICHA_OK);
		}
		FUZZ_ASSERT(ctx.last_raw.len <= SICHA_EXCERPT_MAX);
		sicha_sse_free(&sse);
		sicha_stream_ctx_free(&ctx);
	}

	check_result_invariants(r);
	sicha_result_destroy(r);
	return 0;
}
