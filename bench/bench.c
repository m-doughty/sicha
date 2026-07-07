/* sicha_bench — throughput numbers plus a bitwise counter gate.
 *
 * Two scenarios:
 *   sse    a seeded, integer-deterministic synthetic SSE stream is
 *          pushed through the parser and the stream assembler in
 *          16 KiB feeds; gated on the event count, total delta bytes,
 *          and an FNV-1a digest of the delta byte stream.
 *   build  a fixed chat request (samplers, stops, escapes, extra-body
 *          merge) is serialized repeatedly; gated on the byte length
 *          and FNV-1a digest of the built JSON.
 *
 * The gates are integers that must match BIT FOR BIT on every OS,
 * compiler, and architecture — `sicha_bench --check` is wired into
 * CI on three platforms as a cross-platform determinism check.
 * Timings are printed but never gated. */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sicha_internal.h"

/* ------------------------------------------------------------------ */
/* Deterministic helpers (never clock-seeded)                          */
/* ------------------------------------------------------------------ */

static uint64_t splitmix64(uint64_t *s)
{
	uint64_t z = (*s += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

#define FNV_INIT 0xcbf29ce484222325ull

static uint64_t fnv1a(uint64_t h, const char *bytes, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		h = (h ^ (uint8_t)bytes[i]) * 0x100000001B3ull;
	}
	return h;
}

/* ------------------------------------------------------------------ */
/* Scenario: sse                                                       */
/* ------------------------------------------------------------------ */

#define SSE_EVENTS 100000u
#define SSE_FEED 16384u

/* JSON-escaped token table: ASCII, escapes, \u escapes (multi-byte
 * UTF-8 after decode).  All decoding work is integer-deterministic. */
static const char *SSE_TOKS[] = {
	"Hello", " world", " the", " quick", " brown",
	" fox\\n", " \\\"quoted\\\"", " backs\\\\lash",
	" caf\\u00e9", " \\u2603 snow", " \\ud83d\\ude00",
	" tab\\there", " zero\\u0000byte", " end.",
};

typedef struct sse_gate {
	uint32_t deltas;
	uint64_t delta_bytes;
	uint64_t fnv;
} sse_gate;

typedef struct sse_sink {
	sse_gate g;
} sse_sink;

static int32_t sse_on_delta(void *ud, const char *bytes, size_t len)
{
	sse_sink *s = ud;

	s->g.deltas++;
	s->g.delta_bytes += len;
	s->g.fnv = fnv1a(s->g.fnv, bytes, len);
	return 0;
}

static char *build_sse_stream(size_t *len_out)
{
	sicha_buf b;
	uint64_t rng = 0x5EED5EED5EEDull;

	sicha_buf_init(&b, 0);
	for (uint32_t e = 0; e < SSE_EVENTS; e++) {
		uint32_t n_toks = 1 + (uint32_t)(splitmix64(&rng) % 4);

		sicha_buf_append_cstr(&b,
			"data: {\"choices\":[{\"delta\":{\"content\":\"");
		for (uint32_t t = 0; t < n_toks; t++) {
			sicha_buf_append_cstr(&b, SSE_TOKS[
				splitmix64(&rng) %
				SICHA_COUNTOF(SSE_TOKS)]);
		}
		sicha_buf_append_cstr(&b, "\"}}]}\n\n");
		if (splitmix64(&rng) % 16 == 0) {
			sicha_buf_append_cstr(&b, ": keep-alive\n\n");
		}
	}
	sicha_buf_append_cstr(&b,
		"data: {\"choices\":[{\"delta\":{},\"finish_reason\":"
		"\"stop\"}]}\n\ndata: [DONE]\n\n");
	if (sicha_buf_status(&b) != SICHA_OK) {
		sicha_buf_free(&b);
		return NULL;
	}
	return sicha_buf_take(&b, len_out);
}

static int run_sse(sse_gate *out, double *mb_per_s)
{
	size_t len = 0;
	char *stream = build_sse_stream(&len);
	sicha_result *r = sicha_result_create(0);
	sicha_callbacks cbs;
	sse_sink sink;
	sicha_stream_ctx ctx;
	sicha_sse sse;
	uint64_t t0;
	uint64_t t1;
	int ok = 1;

	if (stream == NULL || r == NULL) {
		free(stream);
		sicha_result_destroy(r);
		return 0;
	}
	memset(&sink, 0, sizeof(sink));
	sink.g.fnv = FNV_INIT;
	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = &sink;
	cbs.on_delta = sse_on_delta;
	/* NO_ACCUMULATE mode: measure parse + dispatch, not memcpy
	 * into the result */
	sicha_stream_ctx_init(&ctx, r, &cbs, 0);
	sicha_sse_init(&sse, 0);

	t0 = sicha_clock_real.now_ms(NULL);
	{
		size_t pos = 0;

		while (pos < len) {
			size_t n = len - pos > SSE_FEED ? SSE_FEED :
				len - pos;
			int32_t rc = sicha_sse_feed(&sse, stream + pos, n,
				sicha_stream_on_event, &ctx);

			if (rc == SICHA_STREAM_STOP) {
				break;
			}
			if (rc != 0) {
				ok = 0;
				break;
			}
			pos += n;
		}
	}
	t1 = sicha_clock_real.now_ms(NULL);

	*out = sink.g;
	*mb_per_s = t1 > t0 ?
		((double)len / (1024.0 * 1024.0)) /
			((double)(t1 - t0) / 1000.0) : 0.0;
	sicha_sse_free(&sse);
	sicha_stream_ctx_free(&ctx);
	sicha_result_destroy(r);
	free(stream);
	return ok && ctx.done;
}

/* ------------------------------------------------------------------ */
/* Scenario: build                                                     */
/* ------------------------------------------------------------------ */

#define BUILD_ITERS 20000u

typedef struct build_gate {
	uint64_t bytes;
	uint64_t fnv;
} build_gate;

static int run_build(build_gate *out, double *builds_per_s)
{
	sicha_backend_desc d;
	sicha_backend b;
	sicha_message msgs[3];
	static const char *stops[2] = { "\n\n", "<|end|>" };
	sicha_request req;
	uint64_t t0;
	uint64_t t1;
	uint64_t fnv = FNV_INIT;
	size_t first_len = 0;
	int ok = 1;

	memset(&d, 0, sizeof(d));
	d.struct_size = (uint32_t)sizeof(d);
	d.base_url = "http://bench.local:5001/v1";
	d.api_key = "sk-bench";
	d.model = "bench-model-32b";
	d.extra_body_json = "{\"min_p\":0.05,\"dry_multiplier\":0.8,"
		"\"temperature\":1.05}";
	if (sicha_backend_init(&b, &d) != SICHA_OK) {
		return 0;
	}
	msgs[0].role = SICHA_ROLE_SYSTEM;
	msgs[0].content = "You are a benchmark. Be fast.";
	msgs[0].content_len = SICHA_LEN_CSTR;
	msgs[0].tool_call_id = NULL;
	msgs[1].role = SICHA_ROLE_USER;
	msgs[1].content = "quotes \" back\\slashes, control \x01, caf"
		"\xC3\xA9, \xE2\x98\x83 and \xF0\x9F\x98\x80 emoji";
	msgs[1].content_len = SICHA_LEN_CSTR;
	msgs[1].tool_call_id = NULL;
	msgs[2].role = SICHA_ROLE_ASSISTANT;
	msgs[2].content = "ready";
	msgs[2].content_len = SICHA_LEN_CSTR;
	msgs[2].tool_call_id = NULL;

	memset(&req, 0, sizeof(req));
	req.struct_size = (uint32_t)sizeof(req);
	req.messages = msgs;
	req.n_messages = SICHA_COUNTOF(msgs);
	req.set_mask = SICHA_SET_MAX_TOKENS | SICHA_SET_TEMPERATURE |
		SICHA_SET_TOP_P | SICHA_SET_TOP_K |
		SICHA_SET_PRESENCE_PENALTY | SICHA_SET_FREQUENCY_PENALTY;
	req.max_tokens = 512;
	req.temperature = 0.7;
	req.top_p = 0.95;
	req.top_k = 40;
	req.presence_penalty = 0.25;
	req.frequency_penalty = -0.5;
	req.stop = stops;
	req.n_stop = 2;
	req.response_format_json = "{\"type\":\"json_schema\","
		"\"json_schema\":{\"name\":\"r\",\"strict\":false,"
		"\"schema\":{\"type\":\"object\"}}}";

	t0 = sicha_clock_real.now_ms(NULL);
	for (uint32_t i = 0; i < BUILD_ITERS; i++) {
		char *json = NULL;
		size_t len = 0;

		if (sicha_build_chat_body(&b, &req, i % 2, &json,
			&len) != SICHA_OK) {
			ok = 0;
			break;
		}
		if (i == 0) {
			first_len = len;
			fnv = fnv1a(fnv, json, len);
		} else if (i == 1) {
			/* the streaming variant gates too */
			fnv = fnv1a(fnv, json, len);
		}
		free(json);
	}
	t1 = sicha_clock_real.now_ms(NULL);

	out->bytes = first_len;
	out->fnv = fnv;
	*builds_per_s = t1 > t0 ?
		(double)BUILD_ITERS / ((double)(t1 - t0) / 1000.0) : 0.0;
	sicha_backend_free(&b);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Baselines (regenerate by running without --check and pasting)       */
/* ------------------------------------------------------------------ */

#define GATE_SSE_DELTAS 100000u
#define GATE_SSE_DELTA_BYTES UINT64_C(1711673)
#define GATE_SSE_FNV UINT64_C(0xDE77AA7362794E73)
#define GATE_BUILD_BYTES UINT64_C(513)
#define GATE_BUILD_FNV UINT64_C(0x89B38FA1F91C0E63)

int main(int argc, char **argv)
{
	int check = argc > 1 && strcmp(argv[1], "--check") == 0;
	sse_gate sg;
	build_gate bg;
	double mbps = 0.0;
	double bps = 0.0;
	int failures = 0;

	if (!run_sse(&sg, &mbps)) {
		fprintf(stderr, "sse scenario failed to run\n");
		return 1;
	}
	if (!run_build(&bg, &bps)) {
		fprintf(stderr, "build scenario failed to run\n");
		return 1;
	}

	printf("sse:   deltas=%" PRIu32 " delta_bytes=%" PRIu64
		" fnv=0x%016" PRIX64 "  (%.1f MB/s)\n",
		sg.deltas, sg.delta_bytes, sg.fnv, mbps);
	printf("build: bytes=%" PRIu64 " fnv=0x%016" PRIX64
		"  (%.0f builds/s)\n",
		bg.bytes, bg.fnv, bps);

	if (check) {
		if (sg.deltas != GATE_SSE_DELTAS) {
			fprintf(stderr, "GATE sse.deltas: got %" PRIu32
				" want %" PRIu32 "\n", sg.deltas,
				GATE_SSE_DELTAS);
			failures++;
		}
		if (sg.delta_bytes != GATE_SSE_DELTA_BYTES) {
			fprintf(stderr, "GATE sse.delta_bytes: got %"
				PRIu64 " want %" PRIu64 "\n",
				sg.delta_bytes, GATE_SSE_DELTA_BYTES);
			failures++;
		}
		if (sg.fnv != GATE_SSE_FNV) {
			fprintf(stderr, "GATE sse.fnv: got 0x%016" PRIX64
				" want 0x%016" PRIX64 "\n", sg.fnv,
				GATE_SSE_FNV);
			failures++;
		}
		if (bg.bytes != GATE_BUILD_BYTES) {
			fprintf(stderr, "GATE build.bytes: got %" PRIu64
				" want %" PRIu64 "\n", bg.bytes,
				GATE_BUILD_BYTES);
			failures++;
		}
		if (bg.fnv != GATE_BUILD_FNV) {
			fprintf(stderr, "GATE build.fnv: got 0x%016"
				PRIX64 " want 0x%016" PRIX64 "\n",
				bg.fnv, GATE_BUILD_FNV);
			failures++;
		}
		if (failures == 0) {
			printf("counter gate: PASS\n");
		}
		return failures == 0 ? 0 : 1;
	}
	return 0;
}
