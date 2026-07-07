/*
 * sicha — LLM chat-completion client.
 *
 * A C17 client library for OpenAI-compatible chat-completion APIs
 * (POST {base_url}/chat/completions): local engines (KoboldCpp,
 * llama.cpp, vLLM, ...), aggregators (OpenRouter), and hosted
 * OAI-compatible endpoints.  Chat completion only — streaming (SSE)
 * and non-streaming — with a retry engine and ordered multi-backend
 * fallback routing built in.  The core library has no system
 * dependencies; HTTP is performed through a pluggable transport
 * vtable (a libcurl transport ships by default, and a fully scripted
 * in-memory transport is part of the public API for testing).
 *
 * Design contract for FFI users (Raku NativeCall, Python cffi, ...):
 *   - Opaque handles + fixed-layout POD structs.  No varargs, no
 *     thread-local error state, no globals.
 *   - All input buffers and structs are fully copied during the call;
 *     sicha never retains a caller pointer (the two documented
 *     exceptions: the transport and clock vtables passed at client
 *     create must outlive the client).
 *   - All returned strings are UTF-8, NUL-terminated, AND carry an
 *     explicit length where embedded NULs are possible.  They are
 *     owned by the object that returned them (result / transport) and
 *     live until that object is destroyed.
 *   - Every fallible function returns a sicha_status (negative =
 *     error).
 *   - Top-level option structs carry a struct_size field for ABI
 *     evolution: set it to sizeof(the struct) as compiled against
 *     this header.  Structs embedded by value (sicha_timeouts,
 *     sicha_retry_policy, sicha_message, sicha_header) are frozen and
 *     carry none; they are versioned by their parent.
 *
 * Retry engine
 * ------------
 * A client owns an ORDERED CHAIN of backends.  Each HTTP round trip
 * is classified into one of three buckets:
 *
 *   ABORT       HTTP 400/401/402/403/404 — configuration or account
 *               errors.  Retrying cannot help; the call fails
 *               immediately with SICHA_E_ABORTED.
 *   RETRY_SAME  Connection failures, TLS failures, resets, 5xx, and
 *               anything unclassifiable — transient trouble.  The
 *               same backend is retried after an exponential backoff:
 *                 wait = min(base * 2^(k-1) + U[0, jitter), cap)
 *               for the k-th same-backend retry (defaults: base 1 s,
 *               jitter 500 ms, cap 30 s).  A Retry-After header (delta
 *               seconds), when present on a RETRY_SAME-classed
 *               response, replaces the formula (capped by
 *               retry_after_cap_ms).  Each backend has max_tries
 *               total attempts (default 3).
 *   ADVANCE     Client-side timeouts, 429, empty or malformed
 *               response bodies, and validator rejections — the model
 *               or provider is misbehaving in a way more attempts
 *               rarely fix.  The engine moves to the NEXT backend in
 *               the chain immediately (no backoff).
 *
 * The classification of any HTTP status can be overridden per client
 * (sicha_retry_policy.overrides) — e.g. {429, SICHA_CLASS_RETRY_SAME}
 * turns rate limits into honored-Retry-After waits instead of
 * fallback.  When the last backend is exhausted the call returns
 * SICHA_E_EXHAUSTED and the result carries a per-attempt record array
 * (sicha_result_attempt) with error class, HTTP status, latency, and
 * a capped raw-body excerpt for response-shaped failures.
 * Cancellation is a distinct terminal status (SICHA_E_CANCELLED),
 * never conflated with exhaustion.
 *
 * An optional validator callback runs on every successful completion;
 * rejection triggers up to validation_retries same-backend re-rolls
 * (no backoff — the point is a fresh sample), then advances.
 *
 * Streaming commit rule: once at least one delta byte has been
 * delivered to the caller, a later failure of that stream terminates
 * with SICHA_E_STREAM_LOST instead of retrying — silently re-running
 * the request would re-deliver text the caller already consumed.
 * SICHA_POLICY_RETRY_AFTER_DELTAS opts back into full retry
 * semantics; an on_attempt record with error_class != SICHA_CLASS_NONE
 * is the caller's signal to discard accumulated state (a retry may
 * follow).  Failures before the first delta always retry normally.
 *
 * Streaming (SSE)
 * ---------------
 * sicha_chat_stream consumes text/event-stream bodies incrementally:
 * events may be split anywhere across TCP reads; CRLF, LF, and CR
 * line endings are accepted; a UTF-8 BOM is stripped; comment lines
 * (": keep-alive") are ignored; multi-line data: fields are joined
 * with '\n'; "data: [DONE]" terminates the stream.  Malformed or
 * unrecognized event payloads are reported through on_raw_chunk and
 * skipped — they never kill the stream.  Delta bytes handed to
 * on_delta are byte-accurate UTF-8 but may split grapheme clusters
 * (and, at chunk boundaries, multi-byte sequences are still delivered
 * whole because event payloads are complete JSON strings).
 *
 * Tool calls (v0.1): requests may declare role TOOL messages and
 * tool_call_id, and finish_reason "tool_calls" is surfaced, but
 * streaming tool-call delta ASSEMBLY is not implemented yet — those
 * payloads arrive via on_raw_chunk, and non-streaming bodies are
 * retained verbatim (sicha_result_raw_body) so nothing is dropped.
 *
 * Threading contract
 * ------------------
 * A client is immutable after create and internally synchronized:
 * any number of threads may run sicha_chat / sicha_chat_stream on the
 * same client concurrently.  Calls are BLOCKING; all callbacks fire
 * on the calling thread — asynchrony is the caller's job (run the
 * call on your own thread).  A cancel token is thread-safe, may be
 * triggered from any thread, and may be shared by many in-flight
 * requests (mass cancel).  A result object is single-threaded.  The
 * transport and clock vtables must be thread-safe and must outlive
 * the client.
 *
 * Timeouts (per backend, milliseconds; 0 = default, SICHA_INFINITE =
 * no limit): connect 30 000, first_byte 60 000 (time to the first
 * response header — set SICHA_INFINITE for local engines that spend
 * minutes in prompt prefill), idle 300 000 (body inactivity), total
 * 1 800 000 (wall clock).  All HTTP is forced to HTTP/1.1: several
 * OAI-compatible gateways hang on HTTP/2 header frames.
 *
 * Security notes
 * --------------
 * api_key is sent as "Authorization: Bearer <key>" and is never
 * written to any log: when wire logging is enabled (environment
 * variable SICHA_DEBUG=<file path, or "-" for stderr>, read once at
 * client create) the Authorization value is redacted.  User-supplied
 * header names/values are rejected if they contain CR, LF, or NUL.
 * Response memory is capped (max_response_bytes, max_event_bytes,
 * and per-transport header caps: 256 headers, 16 KiB each, 64 KiB
 * total).  TLS (certificates, proxies) is delegated entirely to the
 * transport — for the built-in transport, to libcurl and its
 * defaults.
 */

#ifndef SICHA_H
#define SICHA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(SICHA_STATIC)
#define SICHA_API
#elif defined(_WIN32)
#if defined(SICHA_BUILD)
#define SICHA_API __declspec(dllexport)
#else
#define SICHA_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define SICHA_API __attribute__((visibility("default")))
#else
#define SICHA_API
#endif

#define SICHA_VERSION_MAJOR 0
#define SICHA_VERSION_MINOR 1
#define SICHA_VERSION_PATCH 0

/* Runtime version as (major << 16) | (minor << 8) | patch. */
SICHA_API uint32_t sicha_version(void);
SICHA_API const char *sicha_version_str(void);

/* ------------------------------------------------------------------ */
/* Limits and sentinels                                                */
/* ------------------------------------------------------------------ */

/* Pass as a length to mean "the string is NUL-terminated". */
#define SICHA_LEN_CSTR ((size_t)-1)

/* For uint32_t timeout / policy fields: no limit at all.             */
#define SICHA_INFINITE 0xFFFFFFFFu
/* For uint32_t policy fields where the feature can be switched off.  */
#define SICHA_DISABLED 0xFFFFFFFFu

/* Cap on sicha_attempt.raw_body_excerpt, bytes.                      */
#define SICHA_EXCERPT_MAX 2048u

/* Defaults substituted for zeroed fields.                            */
#define SICHA_DEFAULT_CONNECT_MS 30000u
#define SICHA_DEFAULT_FIRST_BYTE_MS 60000u
#define SICHA_DEFAULT_IDLE_MS 300000u
#define SICHA_DEFAULT_TOTAL_MS 1800000u
#define SICHA_DEFAULT_MAX_TRIES 3u
#define SICHA_DEFAULT_BACKOFF_BASE_MS 1000u
#define SICHA_DEFAULT_BACKOFF_CAP_MS 30000u
#define SICHA_DEFAULT_BACKOFF_JITTER_MS 500u
#define SICHA_DEFAULT_RETRY_AFTER_CAP_MS 120000u
#define SICHA_DEFAULT_MAX_RESPONSE_BYTES (64u * 1024u * 1024u)
#define SICHA_DEFAULT_MAX_EVENT_BYTES (1024u * 1024u)

/* ------------------------------------------------------------------ */
/* Status codes                                                        */
/* ------------------------------------------------------------------ */

typedef int32_t sicha_status;

enum {
	SICHA_OK = 0,
	/* Bad pointer, bad struct_size, invalid UTF-8, malformed URL or
	 * header, empty message list, unknown enum value, ...            */
	SICHA_E_INVALID_ARG = -1,
	SICHA_E_NOMEM = -2,
	/* An abort-class response (auth / config error).  See the result
	 * attempt records for the HTTP status and body excerpt.          */
	SICHA_E_ABORTED = -3,
	/* Every backend in the chain was exhausted.                      */
	SICHA_E_EXHAUSTED = -4,
	/* The cancel token fired, or a callback returned nonzero.        */
	SICHA_E_CANCELLED = -5,
	/* A stream failed after delta bytes were already delivered and
	 * SICHA_POLICY_RETRY_AFTER_DELTAS is not set.                    */
	SICHA_E_STREAM_LOST = -6
};

SICHA_API const char *sicha_status_str(sicha_status s);

/* Per-attempt error classification (see the retry-engine spec).      */
typedef int32_t sicha_error_class;

enum {
	SICHA_CLASS_NONE = 0,       /* the attempt succeeded              */
	SICHA_CLASS_ABORT = 1,
	SICHA_CLASS_RETRY_SAME = 2,
	SICHA_CLASS_ADVANCE = 3,
	SICHA_CLASS_CANCELLED = 4,
	SICHA_CLASS_VALIDATION = 5  /* validator callback rejected        */
};

SICHA_API const char *sicha_error_class_str(sicha_error_class c);

typedef int32_t sicha_finish_reason;

enum {
	SICHA_FINISH_UNKNOWN = 0,   /* absent or unrecognized — the raw
	                               string is kept (see
	                               sicha_result_finish_reason_raw)    */
	SICHA_FINISH_STOP = 1,
	SICHA_FINISH_LENGTH = 2,
	SICHA_FINISH_TOOL_CALLS = 3,
	SICHA_FINISH_CONTENT_FILTER = 4
};

SICHA_API const char *sicha_finish_reason_str(sicha_finish_reason f);

/* ------------------------------------------------------------------ */
/* Opaque types                                                        */
/* ------------------------------------------------------------------ */

typedef struct sicha_client sicha_client;
typedef struct sicha_result sicha_result;
typedef struct sicha_cancel sicha_cancel;

/* ------------------------------------------------------------------ */
/* Cancel token                                                        */
/* ------------------------------------------------------------------ */

/* Thread-safe.  One token may be attached to any number of requests
 * (trigger once, cancel them all).  Triggering wakes backoff sleeps
 * instantly and interrupts in-flight I/O within ~100 ms. */
SICHA_API sicha_cancel *sicha_cancel_create(void);
SICHA_API void sicha_cancel_destroy(sicha_cancel *c);       /* NULL ok */
SICHA_API void sicha_cancel_trigger(sicha_cancel *c);
/* Re-arm a triggered token.  Only call when no request using it is
 * in flight. */
SICHA_API void sicha_cancel_reset(sicha_cancel *c);
SICHA_API int32_t sicha_cancel_is_cancelled(const sicha_cancel *c);

/* ------------------------------------------------------------------ */
/* Clock vtable                                                        */
/* ------------------------------------------------------------------ */

/* Injectable time source: retry schedules become exactly reproducible
 * in tests (fake clocks advance virtual time and return instantly).
 * NULL in sicha_client_opts selects the built-in real clock, whose
 * wait blocks on the cancel token's condition variable so triggering
 * wakes it immediately. */
typedef struct sicha_clock {
	void *ud;
	uint64_t (*now_ms)(void *ud);
	/* Block for up to ms (SICHA_INFINITE allowed) or until cancel
	 * fires.  cancel may be NULL.  Return nonzero iff cancelled. */
	int32_t (*wait_ms)(void *ud, sicha_cancel *cancel, uint64_t ms);
} sicha_clock;

/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

typedef struct sicha_header {
	const char *name;
	const char *value;
} sicha_header;

/* Per-request limits, milliseconds.  0 = default, SICHA_INFINITE =
 * no limit. */
typedef struct sicha_timeouts {
	uint32_t connect_ms;
	uint32_t first_byte_ms;
	uint32_t idle_ms;
	uint32_t total_ms;
} sicha_timeouts;

enum {
	/* Best-effort side request (e.g. a cancellation assist): short
	 * budgets, response discarded, failure ignored. */
	SICHA_HTTP_FIRE_AND_FORGET = 1u << 0
};

typedef struct sicha_http_request {
	uint32_t struct_size;
	const char *method;         /* "POST" or "GET"                    */
	const char *url;            /* absolute http(s) URL               */
	const sicha_header *headers;
	size_t n_headers;
	const char *body;           /* NULL for bodyless requests         */
	size_t body_len;
	sicha_timeouts timeouts;    /* fully resolved: ms or INFINITE     */
	uint32_t flags;             /* SICHA_HTTP_*                       */
} sicha_http_request;

/* Receives the response.  on_status fires exactly once, before any
 * on_body; body bytes are delivered as they arrive (streaming).  A
 * nonzero return from either aborts the transfer.  Either member (or
 * the sink itself) may be NULL — the transport then drains silently. */
typedef struct sicha_http_sink {
	void *ud;
	int32_t (*on_status)(void *ud, int32_t http_status,
		const sicha_header *headers, size_t n_headers);
	int32_t (*on_body)(void *ud, const char *bytes, size_t len);
} sicha_http_sink;

typedef int32_t sicha_transport_status;

enum {
	SICHA_T_OK = 0,
	SICHA_T_E_CONNECT = -1,     /* refused / DNS / unreachable        */
	SICHA_T_E_TLS = -2,
	SICHA_T_E_TIMEOUT_CONNECT = -3,
	SICHA_T_E_TIMEOUT_FIRST_BYTE = -4,
	SICHA_T_E_TIMEOUT_IDLE = -5,
	SICHA_T_E_TIMEOUT_TOTAL = -6,
	SICHA_T_E_RESET = -7,       /* reset / premature close            */
	SICHA_T_E_ABORTED_BY_SINK = -8,
	SICHA_T_E_CANCELLED = -9,
	SICHA_T_E_PROTOCOL = -10,   /* malformed HTTP / caps exceeded     */
	SICHA_T_E_NOMEM = -11,
	SICHA_T_E_OTHER = -12
};

SICHA_API const char *sicha_transport_status_str(sicha_transport_status s);

/* A transport must be thread-safe (perform may run concurrently from
 * many threads), must poll the cancel token at ~100 ms granularity,
 * must enforce the response header caps, and must outlive any client
 * it was handed to.  The client never destroys a caller-supplied
 * transport. */
typedef struct sicha_transport {
	uint32_t struct_size;
	void *ud;
	sicha_transport_status (*perform)(void *ud,
		const sicha_http_request *req, const sicha_http_sink *sink,
		sicha_cancel *cancel, const sicha_clock *clock);
} sicha_transport;

/* ------------------------------------------------------------------ */
/* Backends                                                            */
/* ------------------------------------------------------------------ */

enum {
	/* On cancellation, fire-and-forget POST {origin}/api/extra/abort
	 * so a KoboldCpp-style engine stops generating instead of
	 * wasting compute on an abandoned request. */
	SICHA_BACKEND_KOBOLD_CANCEL_ASSIST = 1u << 0,
	/* Send stream_options:{"include_usage":true} on streaming
	 * requests.  Off by default: some gateways hang or error on it. */
	SICHA_BACKEND_STREAM_USAGE = 1u << 1
};

/* One entry in the fallback chain.  struct_size must equal
 * sizeof(sicha_backend_desc) exactly (these live in arrays, so older
 * layouts cannot be accepted — the check catches header/library
 * mismatch). */
typedef struct sicha_backend_desc {
	uint32_t struct_size;
	/* "http[s]://host[:port][/path]" — typically ends in /v1.  The
	 * client appends "/chat/completions".  Trailing slashes are
	 * trimmed. */
	const char *base_url;
	const char *api_key;        /* NULL = no Authorization header     */
	const char *model;          /* required, non-empty                */
	/* Appended after the generated headers (e.g. OpenRouter
	 * HTTP-Referer / X-Title attribution).  Names must not collide
	 * with Content-Type, Accept, User-Agent, or (when api_key is
	 * set) Authorization. */
	const sicha_header *extra_headers;
	size_t n_extra_headers;
	/* Raw JSON object merged into the request-body root — the escape
	 * hatch for provider-specific fields (KoboldCpp min_p / dry_* /
	 * xtc_*, OpenRouter reasoning:{effort}, ...).  On key collision
	 * the extra body wins.  NULL = none. */
	const char *extra_body_json;
	sicha_timeouts timeouts;    /* zeroed = defaults                  */
	uint32_t flags;             /* SICHA_BACKEND_*                    */
} sicha_backend_desc;

/* ------------------------------------------------------------------ */
/* Retry policy                                                        */
/* ------------------------------------------------------------------ */

typedef struct sicha_status_override {
	int32_t http_status;
	sicha_error_class cls;      /* ABORT, RETRY_SAME, or ADVANCE      */
} sicha_status_override;

enum {
	/* Classify finish_reason "length" / "content_filter" as ADVANCE
	 * (pipeline mode) instead of success-with-finish-reason. */
	SICHA_POLICY_LENGTH_IS_ADVANCE = 1u << 0,
	SICHA_POLICY_CONTENT_FILTER_IS_ADVANCE = 1u << 1,
	/* Keep retrying a stream even after deltas were delivered (see
	 * the streaming commit rule above). */
	SICHA_POLICY_RETRY_AFTER_DELTAS = 1u << 2
};

/* Zeroed = all defaults. */
typedef struct sicha_retry_policy {
	uint32_t max_tries;          /* per backend; 0 = default 3        */
	uint32_t validation_retries; /* 0 = default 0                     */
	uint32_t backoff_base_ms;    /* 0 = default 1000                  */
	uint32_t backoff_cap_ms;     /* 0 = default 30000                 */
	uint32_t backoff_jitter_ms;  /* 0 = default 500; SICHA_DISABLED
	                                = no jitter                       */
	uint32_t retry_after_cap_ms; /* 0 = default 120000; SICHA_DISABLED
	                                = ignore Retry-After entirely     */
	uint32_t flags;              /* SICHA_POLICY_*                    */
	const sicha_status_override *overrides;
	size_t n_overrides;
} sicha_retry_policy;

/* ------------------------------------------------------------------ */
/* Client                                                              */
/* ------------------------------------------------------------------ */

typedef struct sicha_client_opts {
	uint32_t struct_size;
	const sicha_backend_desc *backends; /* ordered chain, >= 1        */
	size_t n_backends;
	sicha_retry_policy retry;
	/* NULL = the built-in libcurl transport (SICHA_E_INVALID_ARG if
	 * the library was built without it). */
	const sicha_transport *transport;
	const sicha_clock *clock;   /* NULL = built-in real clock         */
	/* Seed for backoff jitter.  0 = derive a per-client seed from
	 * environmental entropy (NOT cryptographic; merely ensures
	 * independent processes don't retry in lockstep).  Tests pass an
	 * explicit seed for exact reproducibility. */
	uint64_t prng_seed;
	size_t max_response_bytes;  /* 0 = default 64 MiB                 */
	size_t max_event_bytes;     /* single SSE event; 0 = 1 MiB        */
	const char *user_agent;     /* NULL = "sicha/" SICHA_VERSION      */
} sicha_client_opts;

/* Deep-copies opts (backends, strings, headers, policy).  The client
 * is immutable afterwards; only the transport / clock POINTERS are
 * retained.  Validates every backend (URL shape, header injection,
 * UTF-8) up front. */
SICHA_API sicha_status sicha_client_create(const sicha_client_opts *opts,
	sicha_client **out);
/* No calls may be in flight.  NULL ok. */
SICHA_API void sicha_client_destroy(sicha_client *c);

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

enum {
	SICHA_ROLE_SYSTEM = 0,
	SICHA_ROLE_USER = 1,
	SICHA_ROLE_ASSISTANT = 2,
	SICHA_ROLE_TOOL = 3
};

typedef struct sicha_message {
	int32_t role;               /* SICHA_ROLE_*                       */
	const char *content;        /* valid UTF-8; may embed NUL when
	                               content_len is explicit            */
	size_t content_len;         /* SICHA_LEN_CSTR = strlen(content)   */
	const char *tool_call_id;   /* role TOOL only; NULL = omit        */
} sicha_message;

enum {
	SICHA_SET_MAX_TOKENS = 1u << 0,
	SICHA_SET_TEMPERATURE = 1u << 1,
	SICHA_SET_TOP_P = 1u << 2,
	SICHA_SET_TOP_K = 1u << 3,
	SICHA_SET_PRESENCE_PENALTY = 1u << 4,
	SICHA_SET_FREQUENCY_PENALTY = 1u << 5
};

enum {
	/* Streaming: deliver deltas through the callbacks only; skip the
	 * accumulated text / reasoning copies on the result. */
	SICHA_REQ_NO_ACCUMULATE = 1u << 0
};

typedef struct sicha_request {
	uint32_t struct_size;
	/* Exactly one of (messages, n_messages) / messages_json must be
	 * set.  messages_json is a raw JSON ARRAY used verbatim as the
	 * "messages" member — the escape hatch for shapes v0.1 does not
	 * model (content parts / images, assistant tool_calls echo). */
	const sicha_message *messages;
	size_t n_messages;
	const char *messages_json;
	/* Samplers are sent only when their SICHA_SET_* bit is on — a
	 * zeroed struct sends none and the server uses its defaults. */
	uint32_t set_mask;
	int32_t max_tokens;
	double temperature;
	double top_p;
	int32_t top_k;
	double presence_penalty;
	double frequency_penalty;
	const char *const *stop;    /* stop sequences                     */
	size_t n_stop;
	/* Raw JSON used verbatim as the "response_format" member, e.g.
	 * {"type":"json_schema","json_schema":{...}}.  NULL = omit. */
	const char *response_format_json;
	uint32_t flags;             /* SICHA_REQ_*                        */
} sicha_request;

/* ------------------------------------------------------------------ */
/* Attempt records / telemetry                                         */
/* ------------------------------------------------------------------ */

/* One HTTP round trip.  Also the telemetry payload handed to
 * on_attempt.  Strings are owned by the result and live until
 * sicha_result_destroy. */
typedef struct sicha_attempt {
	uint32_t struct_size;
	uint32_t attempt;           /* global ordinal, 0-based            */
	uint32_t backend;           /* index into opts.backends           */
	uint32_t try_of_backend;    /* 0-based                            */
	const char *model;          /* the backend's configured model     */
	sicha_error_class error_class;  /* SICHA_CLASS_NONE on success    */
	int32_t http_status;        /* 0 = none (e.g. connect failure)    */
	sicha_transport_status transport_status;
	uint64_t latency_ms;
	int64_t prompt_tokens;      /* -1 = unknown                       */
	int64_t completion_tokens;
	int64_t total_tokens;
	/* Short human-readable description; never contains credentials. */
	const char *message;
	/* First SICHA_EXCERPT_MAX bytes of the response body, kept only
	 * for advance / validation-class response failures; "" else.    */
	const char *raw_body_excerpt;
	size_t raw_body_excerpt_len;
} sicha_attempt;

/* ------------------------------------------------------------------ */
/* Callbacks                                                           */
/* ------------------------------------------------------------------ */

/* All callbacks fire on the thread that called sicha_chat[_stream].
 * Any member may be NULL.  A nonzero return from on_delta /
 * on_reasoning / on_raw_chunk cancels the request
 * (SICHA_E_CANCELLED).  Byte pointers are valid only for the duration
 * of the callback — copy what you keep.  Callbacks may trigger a
 * cancel token and may read the objects passed to them; they must not
 * destroy the client, result, or cancel token involved in the call,
 * and must not re-enter sicha on the same client from within the
 * callback. */
typedef struct sicha_callbacks {
	uint32_t struct_size;
	void *ud;
	/* Streaming content delta.  Never fires for sicha_chat. */
	int32_t (*on_delta)(void *ud, const char *bytes, size_t len);
	/* Streaming reasoning delta (delta.reasoning /
	 * delta.reasoning_content). */
	int32_t (*on_reasoning)(void *ud, const char *bytes, size_t len);
	/* An SSE data payload sicha did not fully consume: unparseable
	 * fragments, or events carrying members v0.1 does not model
	 * (e.g. tool-call deltas).  Reported, skipped, never fatal. */
	int32_t (*on_raw_chunk)(void *ud, const char *json, size_t len);
	/* Exactly once per HTTP round trip, success or failure, after
	 * classification and before any backoff / advance.  The record
	 * pointer (and its strings) is only guaranteed for the duration
	 * of the callback. */
	void (*on_attempt)(void *ud, const sicha_attempt *a);
	/* Runs on every successful completion (for streams: after
	 * [DONE], on the assembled result).  Return 0 to accept; nonzero
	 * rejects and triggers validation-retry semantics.  Note the
	 * streaming commit rule: rejecting a stream that already
	 * delivered deltas requires SICHA_POLICY_RETRY_AFTER_DELTAS for
	 * a re-roll to happen. */
	int32_t (*validate)(void *ud, const sicha_result *r);
} sicha_callbacks;

/* ------------------------------------------------------------------ */
/* Perform                                                             */
/* ------------------------------------------------------------------ */

/* Blocking.  cbs and cancel may be NULL.  On every return except
 * SICHA_E_INVALID_ARG (and a NOMEM so early the result itself could
 * not be allocated) *out receives a result — populated with the
 * attempt records even on failure — which the caller must destroy. */
SICHA_API sicha_status sicha_chat(sicha_client *c,
	const sicha_request *req, const sicha_callbacks *cbs,
	sicha_cancel *cancel, sicha_result **out);
SICHA_API sicha_status sicha_chat_stream(sicha_client *c,
	const sicha_request *req, const sicha_callbacks *cbs,
	sicha_cancel *cancel, sicha_result **out);

/* ------------------------------------------------------------------ */
/* Results                                                             */
/* ------------------------------------------------------------------ */

SICHA_API void sicha_result_destroy(sicha_result *r);       /* NULL ok */
SICHA_API sicha_status sicha_result_status(const sicha_result *r);
/* Assistant text (accumulated, for streams).  Never NULL; "" when
 * absent.  len out-param optional. */
SICHA_API const char *sicha_result_text(const sicha_result *r,
	size_t *len);
SICHA_API const char *sicha_result_reasoning(const sicha_result *r,
	size_t *len);
SICHA_API sicha_finish_reason sicha_result_finish_reason(
	const sicha_result *r);
/* The exact finish_reason string the server sent ("" if none).       */
SICHA_API const char *sicha_result_finish_reason_raw(
	const sicha_result *r);
SICHA_API int64_t sicha_result_prompt_tokens(const sicha_result *r);
SICHA_API int64_t sicha_result_completion_tokens(const sicha_result *r);
SICHA_API int64_t sicha_result_total_tokens(const sicha_result *r);
/* Model name REPORTED BY THE SERVER ("" if none).                    */
SICHA_API const char *sicha_result_model(const sicha_result *r);
/* Backend index of the final attempt; -1 if no attempt was made.     */
SICHA_API int32_t sicha_result_backend(const sicha_result *r);
/* Non-streaming: the full verbatim response body (so unmodeled
 * members like tool_calls are never dropped).  Streaming: "".        */
SICHA_API const char *sicha_result_raw_body(const sicha_result *r,
	size_t *len);
SICHA_API uint32_t sicha_result_attempt_count(const sicha_result *r);
/* i < attempt_count; valid until destroy.  NULL if out of range.     */
SICHA_API const sicha_attempt *sicha_result_attempt(
	const sicha_result *r, uint32_t i);

/* ------------------------------------------------------------------ */
/* Scripted transport                                                  */
/* ------------------------------------------------------------------ */

/* An in-memory transport for tests (sicha's own, and consumers'):
 * responses are scripted FIFO, every call is recorded, and delays are
 * consumed through the CLIENT'S clock — with a fake clock all four
 * timeout classes are exercised deterministically and instantly.  An
 * empty queue answers SICHA_T_E_CONNECT.  Thread-safe. */

typedef struct sicha_script_response {
	uint32_t struct_size;
	/* SICHA_T_OK: deliver the HTTP response below.  Any error value:
	 * fail with it — after delivering fail_after_bytes body bytes
	 * (0 = fail before the status line). */
	sicha_transport_status status;
	int32_t http_status;        /* e.g. 200, 429                      */
	const sicha_header *headers;/* response headers (Retry-After...)  */
	size_t n_headers;
	const char *body;
	size_t body_len;            /* SICHA_LEN_CSTR ok                  */
	size_t chunk_size;          /* split body every N bytes; 0 = one
	                               single write                       */
	/* Virtual delays, consumed via clock->wait_ms: cross a resolved
	 * request deadline and the exact matching SICHA_T_E_TIMEOUT_*
	 * is returned. */
	uint64_t connect_delay_ms;
	uint64_t first_byte_delay_ms;
	uint64_t per_chunk_delay_ms;
	size_t fail_after_bytes;    /* only when status != SICHA_T_OK     */
} sicha_script_response;

SICHA_API sicha_transport *sicha_script_create(void);
SICHA_API void sicha_script_destroy(sicha_transport *t);    /* NULL ok */
/* Deep-copies r.  Enforces the header caps. */
SICHA_API sicha_status sicha_script_push(sicha_transport *t,
	const sicha_script_response *r);
/* Recorded calls (in perform order).  Strings owned by the transport,
 * valid until destroy; index out of range returns NULL / 0. */
SICHA_API uint32_t sicha_script_call_count(const sicha_transport *t);
SICHA_API const char *sicha_script_call_url(const sicha_transport *t,
	uint32_t i);
SICHA_API const char *sicha_script_call_body(const sicha_transport *t,
	uint32_t i, size_t *len);
/* Case-insensitive header lookup on recorded call i; NULL if absent. */
SICHA_API const char *sicha_script_call_header(const sicha_transport *t,
	uint32_t i, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SICHA_H */
