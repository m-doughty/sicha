/* sse.c: table-driven wire cases (line endings, comments, BOM,
 * multi-line data, lenient EOF, caps, abort propagation) plus a
 * seeded property test: any byte-partition of a stream yields the
 * identical event sequence. */

#include <stdlib.h>

#include "sicha_internal.h"
#include "support.h"

/* ------------------------------------------------------------------ */
/* Event collector                                                     */
/* ------------------------------------------------------------------ */

#define COLLECT_MAX 512

typedef struct collector {
	char *items[COLLECT_MAX];
	size_t lens[COLLECT_MAX];
	size_t n;
} collector;

static int32_t collect(void *ud, const char *data, size_t len)
{
	collector *c = ud;

	if (c->n >= COLLECT_MAX) {
		return -99;
	}
	c->items[c->n] = malloc(len + 1);
	if (c->items[c->n] == NULL) {
		return -98;
	}
	memcpy(c->items[c->n], data, len);
	c->items[c->n][len] = '\0';
	c->lens[c->n] = len;
	c->n++;
	return 0;
}

static void collector_reset(collector *c)
{
	for (size_t i = 0; i < c->n; i++) {
		free(c->items[i]);
	}
	memset(c, 0, sizeof(*c));
}

static int collector_eq(const collector *a, const collector *b)
{
	if (a->n != b->n) {
		return 0;
	}
	for (size_t i = 0; i < a->n; i++) {
		if (a->lens[i] != b->lens[i] ||
			memcmp(a->items[i], b->items[i], a->lens[i]) != 0) {
			return 0;
		}
	}
	return 1;
}

/* Parse `input` in one shot (feed + finish) into `out`. */
static int32_t parse_all(const char *input, size_t len, collector *out)
{
	sicha_sse p;
	int32_t rc;

	sicha_sse_init(&p, 0);
	rc = sicha_sse_feed(&p, input, len, collect, out);
	if (rc == 0) {
		rc = sicha_sse_finish(&p, collect, out);
	}
	sicha_sse_free(&p);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Table cases                                                         */
/* ------------------------------------------------------------------ */

typedef struct table_case {
	const char *name;
	const char *input;      /* NUL-terminated for convenience       */
	const char *expect[8];  /* NULL-terminated list                 */
} table_case;

static const table_case CASES[] = {
	{ "single event", "data: hello\n\n", { "hello", NULL } },
	{ "two events", "data: 1\n\ndata: 2\n\n", { "1", "2", NULL } },
	{ "multi-line data joined with newline",
		"data: a\ndata: b\n\n", { "a\nb", NULL } },
	{ "no space after colon", "data:raw\n\n", { "raw", NULL } },
	{ "only one leading space stripped",
		"data:  padded\n\n", { " padded", NULL } },
	{ "bare data field is an empty line",
		"data\n\n", { "", NULL } },
	{ "bare data lines join",
		"data\ndata\n\n", { "\n", NULL } },
	{ "comment ignored", ": keep-alive\ndata: x\n\n", { "x", NULL } },
	{ "openrouter heartbeat",
		": OPENROUTER PROCESSING\n\ndata: x\n\n", { "x", NULL } },
	{ "crlf endings", "data: x\r\n\r\n", { "x", NULL } },
	{ "bare cr endings", "data: x\r\r", { "x", NULL } },
	{ "mixed endings",
		"data: a\r\ndata: b\n\r\n", { "a\nb", NULL } },
	{ "non-data fields ignored",
		"event: message\nid: 7\nretry: 100\ndata: x\n\n",
		{ "x", NULL } },
	{ "unknown field ignored",
		"datum: nope\ndata: x\n\n", { "x", NULL } },
	{ "field name is case-sensitive per spec",
		"DATA: nope\ndata: x\n\n", { "x", NULL } },
	{ "done sentinel passes through verbatim",
		"data: [DONE]\n\n", { "[DONE]", NULL } },
	{ "bom stripped", "\xEF\xBB\xBF" "data: x\n\n", { "x", NULL } },
	{ "false bom prefix is payload",
		"\xEF\xBB" "data: x\n\ndata: y\n\n", { "y", NULL } },
	{ "blank lines without data emit nothing",
		"\n\n\n\ndata: x\n\n\n\n", { "x", NULL } },
	{ "comment-only stream", ": a\n: b\n\n", { NULL } },
	{ "empty input", "", { NULL } },
	{ "lenient eof: missing final blank line",
		"data: x\n", { "x", NULL } },
	{ "lenient eof: unterminated final line",
		"data: x", { "x", NULL } },
	{ "lenient eof: multi-line pending event",
		"data: a\ndata: b", { "a\nb", NULL } },
	{ "eof mid non-data field emits nothing", "dat", { NULL } },
	{ "eof mid comment emits nothing", ": trailing", { NULL } },
	{ "colon in value survives",
		"data: {\"a\": 1}\n\n", { "{\"a\": 1}", NULL } },
};

static void check_table(void)
{
	for (size_t i = 0; i < SICHA_COUNTOF(CASES); i++) {
		const table_case *tc = &CASES[i];
		collector got = { { 0 }, { 0 }, 0 };
		size_t expect_n = 0;
		int32_t rc = parse_all(tc->input, strlen(tc->input), &got);

		T_CHECK(rc == 0);
		while (tc->expect[expect_n] != NULL) {
			expect_n++;
		}
		t_check_impl(got.n == expect_n, tc->name, __FILE__,
			__LINE__);
		for (size_t k = 0; k < expect_n && k < got.n; k++) {
			t_check_impl(strcmp(got.items[k],
				tc->expect[k]) == 0, tc->name, __FILE__,
				__LINE__);
		}
		collector_reset(&got);
	}
}

/* ------------------------------------------------------------------ */
/* Split-anywhere cases the table cannot express                       */
/* ------------------------------------------------------------------ */

static void check_split_crlf(void)
{
	/* CR and LF of one terminator arriving in separate feeds must
	 * not produce a phantom blank line (which would dispatch). */
	sicha_sse p;
	collector got = { { 0 }, { 0 }, 0 };

	sicha_sse_init(&p, 0);
	T_CHECK(sicha_sse_feed(&p, "data: a\r", 8, collect, &got) == 0);
	T_CHECK(sicha_sse_feed(&p, "\ndata: b\r\n\r", 11, collect,
		&got) == 0);
	T_CHECK(sicha_sse_feed(&p, "\n", 1, collect, &got) == 0);
	T_CHECK(sicha_sse_finish(&p, collect, &got) == 0);
	T_CHECK(got.n == 1);
	T_CHECK(got.n == 1 && strcmp(got.items[0], "a\nb") == 0);
	sicha_sse_free(&p);
	collector_reset(&got);
}

static void check_split_bom(void)
{
	sicha_sse p;
	collector got = { { 0 }, { 0 }, 0 };

	sicha_sse_init(&p, 0);
	T_CHECK(sicha_sse_feed(&p, "\xEF", 1, collect, &got) == 0);
	T_CHECK(sicha_sse_feed(&p, "\xBB", 1, collect, &got) == 0);
	T_CHECK(sicha_sse_feed(&p, "\xBF" "data: x\n\n", 10, collect,
		&got) == 0);
	T_CHECK(sicha_sse_finish(&p, collect, &got) == 0);
	T_CHECK(got.n == 1 && strcmp(got.items[0], "x") == 0);
	sicha_sse_free(&p);
	collector_reset(&got);
}

static void check_split_false_bom_at_eof(void)
{
	/* A held-back partial BOM at EOF is replayed as payload. */
	sicha_sse p;
	collector got = { { 0 }, { 0 }, 0 };

	sicha_sse_init(&p, 0);
	T_CHECK(sicha_sse_feed(&p, "\xEF\xBB", 2, collect, &got) == 0);
	T_CHECK(sicha_sse_finish(&p, collect, &got) == 0);
	/* line "\xEF\xBB" is a nameless field: ignored, no event */
	T_CHECK(got.n == 0);
	sicha_sse_free(&p);
	collector_reset(&got);
}

static void check_binary_payload(void)
{
	/* NUL bytes inside a data line are preserved with exact length */
	static const char input[] = "data: a\0b\n\n";
	collector got = { { 0 }, { 0 }, 0 };

	T_CHECK(parse_all(input, sizeof(input) - 1, &got) == 0);
	T_CHECK(got.n == 1);
	T_CHECK(got.n == 1 && got.lens[0] == 3);
	T_CHECK(got.n == 1 && memcmp(got.items[0], "a\0b", 3) == 0);
	collector_reset(&got);
}

static void check_event_cap(void)
{
	sicha_sse p;
	collector got = { { 0 }, { 0 }, 0 };
	char big[64];

	memset(big, 'x', sizeof(big));
	sicha_sse_init(&p, 16);
	T_CHECK(sicha_sse_feed(&p, "data: ", 6, collect, &got) == 0);
	T_CHECK(sicha_sse_feed(&p, big, sizeof(big), collect,
		&got) == -1);
	T_CHECK(sicha_sse_status(&p) == SICHA_INT_E_TOOBIG);
	/* sticky: further feeds fail immediately */
	T_CHECK(sicha_sse_feed(&p, "y", 1, collect, &got) == -1);
	T_CHECK(sicha_sse_finish(&p, collect, &got) == -1);
	T_CHECK(got.n == 0);
	sicha_sse_free(&p);
	collector_reset(&got);
}

static int32_t abort_after_first(void *ud, const char *data, size_t len)
{
	int *count = ud;

	(void)data;
	(void)len;
	(*count)++;
	return *count >= 1 ? 7 : 0;
}

static void check_abort_propagation(void)
{
	sicha_sse p;
	int count = 0;

	sicha_sse_init(&p, 0);
	T_CHECK(sicha_sse_feed(&p, "data: 1\n\ndata: 2\n\n", 18,
		abort_after_first, &count) == 7);
	T_CHECK(count == 1);
	T_CHECK(sicha_sse_status(&p) == SICHA_OK); /* not an error */
	sicha_sse_free(&p);
}

static void check_null_on_event(void)
{
	/* NULL on_event just discards events */
	sicha_sse p;

	sicha_sse_init(&p, 0);
	T_CHECK(sicha_sse_feed(&p, "data: x\n\n", 9, NULL, NULL) == 0);
	T_CHECK(sicha_sse_finish(&p, NULL, NULL) == 0);
	sicha_sse_free(&p);
}

/* ------------------------------------------------------------------ */
/* Property: partition invariance                                      */
/* ------------------------------------------------------------------ */

static void append_bytes(char *dst, size_t *len, const char *src,
	size_t n)
{
	memcpy(dst + *len, src, n);
	*len += n;
}

static void check_partition_invariance(void)
{
	t_rng rng = { 0xC0FFEE };
	static const char *terminators[] = { "\n", "\r\n", "\r" };
	/* payload alphabet: ASCII, multi-byte UTF-8, colons, spaces */
	static const char *alphabet[] = { "a", "Z", "0", " ", ":", "{",
		"\"", "\xC3\xA9" /* é */, "\xE2\x98\x83" /* ☃ */, "}" };

	for (int iter = 0; iter < 200; iter++) {
		char stream[4096];
		size_t stream_len = 0;
		collector expect = { { 0 }, { 0 }, 0 };
		collector whole = { { 0 }, { 0 }, 0 };
		collector split = { { 0 }, { 0 }, 0 };
		uint32_t n_events = 1 + t_rng_below(&rng, 6);

		if (t_rng_below(&rng, 4) == 0) {
			append_bytes(stream, &stream_len,
				"\xEF\xBB\xBF", 3);
		}
		for (uint32_t e = 0; e < n_events; e++) {
			const char *term = terminators[t_rng_below(&rng, 3)];
			uint32_t n_lines = 1 + t_rng_below(&rng, 3);
			char event_data[512];
			size_t event_len = 0;

			if (t_rng_below(&rng, 3) == 0) {
				append_bytes(stream, &stream_len,
					": beat", 6);
				append_bytes(stream, &stream_len, term,
					strlen(term));
			}
			if (t_rng_below(&rng, 4) == 0) {
				append_bytes(stream, &stream_len,
					"id: 1", 5);
				append_bytes(stream, &stream_len, term,
					strlen(term));
			}
			for (uint32_t l = 0; l < n_lines; l++) {
				uint32_t n_toks = t_rng_below(&rng, 12);

				if (l > 0) {
					event_data[event_len++] = '\n';
				}
				append_bytes(stream, &stream_len,
					"data: ", 6);
				for (uint32_t t = 0; t < n_toks; t++) {
					const char *tok = alphabet[
						t_rng_below(&rng, 10)];

					append_bytes(stream, &stream_len,
						tok, strlen(tok));
					append_bytes(event_data, &event_len,
						tok, strlen(tok));
				}
				append_bytes(stream, &stream_len, term,
					strlen(term));
			}
			append_bytes(stream, &stream_len, term,
				strlen(term));
			(void)collect(&expect, event_data, event_len);
		}

		/* whole-stream parse matches the constructed events */
		T_CHECK(parse_all(stream, stream_len, &whole) == 0);
		T_CHECK(collector_eq(&whole, &expect));

		/* any random partition matches too */
		{
			sicha_sse p;
			size_t pos = 0;
			int ok = 1;

			sicha_sse_init(&p, 0);
			while (pos < stream_len) {
				size_t n = 1 + t_rng_below(&rng,
					(uint32_t)(stream_len - pos > 40 ?
						40 : stream_len - pos));

				if (sicha_sse_feed(&p, stream + pos, n,
					collect, &split) != 0) {
					ok = 0;
					break;
				}
				pos += n;
			}
			if (ok) {
				ok = sicha_sse_finish(&p, collect,
					&split) == 0;
			}
			T_CHECK(ok);
			T_CHECK(collector_eq(&split, &expect));
			sicha_sse_free(&p);
		}

		collector_reset(&expect);
		collector_reset(&whole);
		collector_reset(&split);
	}
}

int main(void)
{
	check_table();
	check_split_crlf();
	check_split_bom();
	check_split_false_bom_at_eof();
	check_binary_payload();
	check_event_cap();
	check_abort_propagation();
	check_null_on_event();
	check_partition_invariance();
	return t_done("test_sse");
}
