/* Shared internals.  Not installed; white-box tests may include it. */

#ifndef SICHA_INTERNAL_H
#define SICHA_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "sicha.h"

#define SICHA_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

/* Response header caps, enforced by every transport. */
#define SICHA_MAX_HEADERS 256u
#define SICHA_MAX_HEADER_BYTES (16u * 1024u)
#define SICHA_MAX_HEADERS_TOTAL_BYTES (64u * 1024u)

/* Internal-only status: a configured size cap was exceeded.  Never
 * escapes the library — the classifier maps it to an advance-class
 * attempt failure. */
#define SICHA_INT_E_TOOBIG ((sicha_status)-100)

/* ------------------------------------------------------------------ */
/* buf.c — growable byte buffer                                        */
/*                                                                     */
/* Invariants: data[len] == '\0' whenever data != NULL; every append   */
/* is overflow-checked against SIZE_MAX and the optional max_len cap;  */
/* errors are STICKY (later appends are no-ops) so call sites check    */
/* once via sicha_buf_status.                                          */
/* ------------------------------------------------------------------ */

typedef struct sicha_buf {
	char *data;
	size_t len;
	size_t cap;         /* usable bytes, excluding the NUL slot       */
	size_t max_len;     /* 0 = unlimited                              */
	sicha_status err;   /* OK, E_NOMEM, or INT_E_TOOBIG (sticky)      */
} sicha_buf;

void sicha_buf_init(sicha_buf *b, size_t max_len);
void sicha_buf_free(sicha_buf *b);
sicha_status sicha_buf_status(const sicha_buf *b);
void sicha_buf_append(sicha_buf *b, const char *bytes, size_t len);
void sicha_buf_append_cstr(sicha_buf *b, const char *s);
void sicha_buf_append_ch(sicha_buf *b, char ch);
/* len = 0, allocation kept, sticky error kept. */
void sicha_buf_reset(sicha_buf *b);
/* Detach the malloc'd, NUL-terminated data (never NULL on success —
 * an empty buffer yields a malloc'd "").  NULL on prior error or
 * allocation failure.  The buffer is re-initialized either way. */
char *sicha_buf_take(sicha_buf *b, size_t *len);

/* ------------------------------------------------------------------ */
/* sse.c — incremental Server-Sent-Events parser                       */
/*                                                                     */
/* Generic SSE (no OpenAI semantics): feed bytes in arbitrary          */
/* partitions, complete events come out.  data: lines accumulate and   */
/* join with '\n'; one leading space after the colon is stripped;      */
/* comment lines and non-data fields are ignored; CRLF / LF / CR all   */
/* terminate lines (CR+LF split across feeds handled); a UTF-8 BOM at  */
/* stream start is stripped even when split across feeds.  A blank     */
/* line dispatches the pending event.  sicha_sse_finish flushes a      */
/* complete pending event at clean EOF (lenient: some gateways omit    */
/* the final blank line).                                              */
/*                                                                     */
/* on_event returning nonzero stops parsing; the feed call returns     */
/* that value.  Feed/finish return 0 on success, an on_event nonzero   */
/* verbatim, or -1 on internal error (see sicha_sse_status: E_NOMEM    */
/* or INT_E_TOOBIG when an event exceeds max_event_bytes).             */
/* ------------------------------------------------------------------ */

typedef int32_t (*sicha_sse_on_event)(void *ud, const char *data,
	size_t len);

/* ------------------------------------------------------------------ */
/* classify.c — policy resolution, error classification, backoff math  */
/* ------------------------------------------------------------------ */

/* 2xx body condition, input to classification. */
enum {
	SICHA_BODY_OK = 0,
	SICHA_BODY_EMPTY = 1,
	SICHA_BODY_MALFORMED = 2,
	SICHA_BODY_TOOBIG = 3
};

/* Substitute defaults for zeroed fields, in place. */
void sicha_policy_resolve(sicha_retry_policy *p);
/* Reject unknown flags / override classes / zero override statuses. */
sicha_status sicha_policy_validate(const sicha_retry_policy *p);

/* Pure, total classification of one HTTP round trip.  `resolved` must
 * have been through sicha_policy_resolve.  The engine intercepts
 * SICHA_T_E_NOMEM (terminal) before consulting the class; classify
 * still answers ADVANCE for it defensively. */
sicha_error_class sicha_classify(const sicha_retry_policy *resolved,
	sicha_transport_status ts, int32_t http_status, int32_t body_state,
	sicha_finish_reason finish);

/* Retry-After header, delta-seconds form only ("120").  Leading and
 * trailing OWS tolerated.  Returns milliseconds saturated at 10^15,
 * or UINT64_MAX when absent / HTTP-date / garbage (caller falls back
 * to the backoff formula). */
uint64_t sicha_retry_after_ms(const char *value, size_t len);

/* Backoff for the k-th same-backend retry (k >= 1):
 *   min(base * 2^(k-1) + jitter_rand % jitter, cap)
 * jitter_rand is raw 64-bit randomness; jitter SICHA_DISABLED = none.
 * Overflow saturates at cap. */
uint64_t sicha_backoff_ms(const sicha_retry_policy *resolved,
	uint32_t k, uint64_t jitter_rand);

/* ------------------------------------------------------------------ */
/* request.c — validation, URLs, headers, chat body serialization      */
/* ------------------------------------------------------------------ */

/* Strict UTF-8 validation (rejects overlongs, surrogates, > U+10FFFF).*/
int sicha_utf8_valid(const char *bytes, size_t len);

/* base_url: http(s)://, non-empty host, no control bytes / space /
 * '#' / '?'. */
sicha_status sicha_url_validate(const char *base_url);
/* "{base minus trailing slashes}{path}", malloc'd; NULL on OOM. */
char *sicha_url_join(const char *base_url, const char *path);
/* "scheme://host[:port]", malloc'd; NULL on OOM. */
char *sicha_url_origin(const char *base_url);

int sicha_header_name_eq(const char *a, const char *b); /* case-insens */
/* name: RFC 7230 token; value: no CR / LF / NUL / other C0 (tab ok). */
sicha_status sicha_header_validate(const char *name, const char *value);

/* A validated, deep-copied, resolved backend (internal form of
 * sicha_backend_desc). */
typedef struct sicha_backend {
	char *base_url;
	char *url_chat;             /* {base}/chat/completions            */
	char *url_abort;            /* {origin}/api/extra/abort, or NULL
	                               when the flag is off               */
	char *api_key;              /* NULL = none                        */
	char *model;
	sicha_header *extra_headers;/* deep copies                        */
	size_t n_extra_headers;
	char *extra_body_json;      /* validated: parses as a JSON object */
	sicha_timeouts timeouts;    /* resolved: ms or SICHA_INFINITE     */
	uint32_t flags;
} sicha_backend;

sicha_status sicha_backend_init(sicha_backend *b,
	const sicha_backend_desc *desc);
void sicha_backend_free(sicha_backend *b);      /* safe on zeroed      */

/* Full request header set for one attempt: Content-Type, Accept,
 * User-Agent, Authorization (when api_key), then the extras.  All
 * strings and the array are malloc'd; free with sicha_headers_free. */
sicha_status sicha_build_headers(const sicha_backend *b, int stream,
	const char *user_agent, sicha_header **out, size_t *n_out);
void sicha_headers_free(sicha_header *h, size_t n);

/* Complete request validation (structure, UTF-8, JSON well-formedness
 * of the passthrough fields).  After this passes, body building can
 * only fail on OOM. */
sicha_status sicha_request_validate(const sicha_request *req);

/* Serialize the chat-completion body for one backend.  *out is
 * malloc'd, NUL-terminated JSON of *out_len bytes. */
sicha_status sicha_build_chat_body(const sicha_backend *b,
	const sicha_request *req, int stream, char **out, size_t *out_len);

/* ------------------------------------------------------------------ */
/* response.c — result object and response assembly                    */
/* ------------------------------------------------------------------ */

struct sicha_result {
	sicha_status status;
	sicha_buf text;
	sicha_buf reasoning;
	char *finish_raw;           /* NULL until seen                    */
	sicha_finish_reason finish;
	int64_t prompt_tokens;      /* -1 unknown                         */
	int64_t completion_tokens;
	int64_t total_tokens;
	char *model;                /* server-reported, NULL until seen   */
	int32_t backend;            /* -1 until an attempt ran            */
	char *raw_body;             /* non-streaming verbatim body        */
	size_t raw_body_len;
	sicha_attempt *attempts;    /* strings deep-copied per record     */
	uint32_t n_attempts;
	uint32_t cap_attempts;
};

sicha_result *sicha_result_create(size_t max_response_bytes);
/* Deep-copies a (NULL string fields tolerated).  On NOMEM the record
 * is dropped and NOMEM returned; the result stays destroyable. */
sicha_status sicha_result_add_attempt(sicha_result *r,
	const sicha_attempt *a);
/* Clear per-attempt content (text, reasoning, finish, usage, model,
 * raw body) for a fresh attempt; attempt records persist. */
void sicha_result_reset_content(sicha_result *r);
sicha_status sicha_result_set_model(sicha_result *r, const char *s,
	size_t len);
sicha_status sicha_result_set_finish(sicha_result *r, const char *raw,
	size_t len);

/* Parse a NON-STREAMING chat-completion body into r.  Returns
 * SICHA_OK/SICHA_E_NOMEM; wire trouble is reported via *body_state
 * (SICHA_BODY_*) for classification, never as a hard error. */
sicha_status sicha_response_parse(sicha_result *r, const char *body,
	size_t len, int32_t *body_state);

/* Streaming assembly context: one per attempt.  Feed SSE event
 * payloads through sicha_stream_on_event (sicha_sse_on_event
 * compatible).  Return codes: 0 = continue, SICHA_STREAM_STOP after
 * [DONE], SICHA_STREAM_CANCEL when a callback returned nonzero, -1
 * on internal error (see ctx.err). */
#define SICHA_STREAM_STOP 1
#define SICHA_STREAM_CANCEL 2

typedef struct sicha_stream_ctx {
	sicha_result *r;
	const sicha_callbacks *cbs; /* may be NULL                        */
	int accumulate;
	int done;                   /* saw [DONE]                         */
	int delivered;              /* callback received delta bytes      */
	int saw_event;              /* any parseable event arrived        */
	sicha_status err;
	/* last payload forwarded to on_raw_chunk, capped — surfaces
	 * mid-stream {"error":...} fragments in attempt records */
	sicha_buf last_raw;
} sicha_stream_ctx;

void sicha_stream_ctx_init(sicha_stream_ctx *ctx, sicha_result *r,
	const sicha_callbacks *cbs, int accumulate);
void sicha_stream_ctx_free(sicha_stream_ctx *ctx);
int32_t sicha_stream_on_event(void *ud, const char *data, size_t len);

/* ------------------------------------------------------------------ */
/* cancel.c — cancel token internals and the built-in real clock       */
/* ------------------------------------------------------------------ */

/* The built-in wall-time clock: now_ms is monotonic; wait_ms blocks
 * on the token's condition variable so sicha_cancel_trigger wakes it
 * immediately (or chunk-sleeps when no token is given). */
extern const sicha_clock sicha_clock_real;

static inline const sicha_clock *sicha_clock_resolve(const sicha_clock *c)
{
	return c != NULL ? c : &sicha_clock_real;
}

/* ------------------------------------------------------------------ */
/* transport_curl.c — built-in libcurl transport (SICHA_WITH_CURL)     */
/* ------------------------------------------------------------------ */

#if defined(SICHA_WITH_CURL)
sicha_transport *sicha_curl_create(void);
void sicha_curl_destroy(sicha_transport *t);     /* NULL ok */
#endif

typedef struct sicha_sse {
	sicha_buf line;     /* partial line across feeds                  */
	sicha_buf data;     /* accumulated data: lines of pending event   */
	int have_data;      /* pending event exists (data buf may be "")  */
	int last_was_cr;    /* swallow an LF that follows a boundary CR   */
	uint8_t bom_seen;   /* bytes of a potential BOM matched so far    */
	int bom_decided;    /* BOM handling finished                      */
	size_t max_event_bytes;
	sicha_status err;   /* sticky                                     */
} sicha_sse;

void sicha_sse_init(sicha_sse *p, size_t max_event_bytes);
void sicha_sse_free(sicha_sse *p);
sicha_status sicha_sse_status(const sicha_sse *p);
int32_t sicha_sse_feed(sicha_sse *p, const char *bytes, size_t len,
	sicha_sse_on_event on_event, void *ud);
int32_t sicha_sse_finish(sicha_sse *p, sicha_sse_on_event on_event,
	void *ud);

/* ------------------------------------------------------------------ */
/* Atomics shim (32-bit): C11 stdatomic everywhere except MSVC, which
 * lacks it in C mode — Interlocked* there.  Sequentially consistent
 * semantics on both paths; nothing here is hot enough to relax.      */
/* ------------------------------------------------------------------ */

#if defined(_MSC_VER) && !defined(__clang__)

#include <intrin.h>

typedef volatile long sicha_atomic_u32;

static __inline uint32_t sicha_atomic_load_u32(sicha_atomic_u32 *p)
{
	return (uint32_t)_InterlockedCompareExchange(p, 0, 0);
}

static __inline void sicha_atomic_store_u32(sicha_atomic_u32 *p,
	uint32_t v)
{
	_InterlockedExchange(p, (long)v);
}

static __inline uint32_t sicha_atomic_fetch_add_u32(sicha_atomic_u32 *p,
	uint32_t v)
{
	return (uint32_t)_InterlockedExchangeAdd(p, (long)v);
}

#else

#include <stdatomic.h>

typedef _Atomic uint32_t sicha_atomic_u32;

static inline uint32_t sicha_atomic_load_u32(sicha_atomic_u32 *p)
{
	return atomic_load(p);
}

static inline void sicha_atomic_store_u32(sicha_atomic_u32 *p,
	uint32_t v)
{
	atomic_store(p, v);
}

static inline uint32_t sicha_atomic_fetch_add_u32(sicha_atomic_u32 *p,
	uint32_t v)
{
	return atomic_fetch_add(p, v);
}

#endif

#endif /* SICHA_INTERNAL_H */
