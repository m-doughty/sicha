/* The client and the retry / fallback engine:
 *
 *   INIT -> ATTEMPT -> CLASSIFY -> { VALIDATE | BACKOFF | ADVANCE |
 *                                    DONE }
 *
 * ported from LLM::Data::Inference::Task.  Per backend, RETRY_SAME
 * failures consume a max_tries attempt budget with exponential
 * jittered backoff (a Retry-After header replaces the formula);
 * ADVANCE failures move down the chain immediately; ABORT failures
 * end the call; validator rejections re-roll the same backend without
 * backoff.  Exactly one sicha_attempt record (and one on_attempt
 * callback) per HTTP round trip.
 *
 * Streaming commit rule: once a callback has received delta bytes,
 * any further retry/advance decision becomes SICHA_E_STREAM_LOST
 * unless SICHA_POLICY_RETRY_AFTER_DELTAS is set.  Accumulation-only
 * streams (no delta callbacks registered) retry freely: the caller
 * never saw partial text and the result content resets per attempt.
 *
 * Wire debug logging: SICHA_DEBUG=<path|-> (read once at client
 * create).  Authorization values are always redacted. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sicha_internal.h"
#include "sicha_thread.h"

struct sicha_client {
	sicha_backend *backends;
	size_t n_backends;
	sicha_retry_policy policy;          /* resolved                */
	sicha_status_override *overrides;   /* deep copy               */
	const sicha_transport *transport;
	const sicha_clock *clock;
	uint64_t prng_seed;
	sicha_atomic_u32 req_counter;
	size_t max_response_bytes;
	size_t max_event_bytes;
	char *user_agent;
	FILE *debug;                        /* NULL = disabled         */
	int debug_owned;                    /* fclose at destroy       */
	sicha_mutex debug_mu;
#if defined(SICHA_WITH_CURL)
	sicha_transport *curl_transport;    /* built-in, owned         */
#endif
};

static uint64_t splitmix64(uint64_t *s)
{
	uint64_t z = (*s += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

/* ------------------------------------------------------------------ */
/* Debug log (redacted)                                                */
/* ------------------------------------------------------------------ */

static void debug_request(sicha_client *c, const sicha_http_request *rq)
{
	if (c->debug == NULL) {
		return;
	}
	sicha_mutex_lock(&c->debug_mu);
	fprintf(c->debug, "=== sicha request ===\n%s %s\n", rq->method,
		rq->url);
	for (size_t i = 0; i < rq->n_headers; i++) {
		if (sicha_header_name_eq(rq->headers[i].name,
				"Authorization")) {
			fprintf(c->debug, "  %s: Bearer ***\n",
				rq->headers[i].name);
		} else {
			fprintf(c->debug, "  %s: %s\n",
				rq->headers[i].name, rq->headers[i].value);
		}
	}
	if (rq->body != NULL) {
		fprintf(c->debug, "%.*s\n", (int)rq->body_len, rq->body);
	}
	fflush(c->debug);
	sicha_mutex_unlock(&c->debug_mu);
}

static void debug_attempt(sicha_client *c, const sicha_attempt *a)
{
	if (c->debug == NULL) {
		return;
	}
	sicha_mutex_lock(&c->debug_mu);
	fprintf(c->debug,
		"=== sicha attempt %u (backend %u try %u) "
		"class=%s transport=%s http=%d latency=%ums: %s\n",
		a->attempt, a->backend, a->try_of_backend,
		sicha_error_class_str(a->error_class),
		sicha_transport_status_str(a->transport_status),
		(int)a->http_status, (unsigned)a->latency_ms, a->message);
	if (a->raw_body_excerpt_len > 0) {
		fprintf(c->debug, "  body: %.*s\n",
			(int)a->raw_body_excerpt_len, a->raw_body_excerpt);
	}
	fflush(c->debug);
	sicha_mutex_unlock(&c->debug_mu);
}

/* ------------------------------------------------------------------ */
/* Client lifecycle                                                    */
/* ------------------------------------------------------------------ */

sicha_status sicha_client_create(const sicha_client_opts *opts,
	sicha_client **out)
{
	sicha_client *c;
	sicha_status st;

	if (out == NULL) {
		return SICHA_E_INVALID_ARG;
	}
	*out = NULL;
	if (opts == NULL || opts->struct_size != (uint32_t)sizeof(*opts) ||
		opts->backends == NULL || opts->n_backends == 0) {
		return SICHA_E_INVALID_ARG;
	}
	st = sicha_policy_validate(&opts->retry);
	if (st != SICHA_OK) {
		return st;
	}
	if (opts->transport != NULL) {
		if (opts->transport->struct_size !=
				(uint32_t)sizeof(*opts->transport) ||
			opts->transport->perform == NULL) {
			return SICHA_E_INVALID_ARG;
		}
	} else {
#if !defined(SICHA_WITH_CURL)
		return SICHA_E_INVALID_ARG; /* built without libcurl */
#endif
	}
	if (opts->clock != NULL && (opts->clock->now_ms == NULL ||
		opts->clock->wait_ms == NULL)) {
		return SICHA_E_INVALID_ARG;
	}
	if (opts->user_agent != NULL &&
		sicha_header_validate("User-Agent",
			opts->user_agent) != SICHA_OK) {
		return SICHA_E_INVALID_ARG;
	}

	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		return SICHA_E_NOMEM;
	}
	sicha_mutex_init(&c->debug_mu);
	c->backends = calloc(opts->n_backends, sizeof(*c->backends));
	if (c->backends == NULL) {
		st = SICHA_E_NOMEM;
		goto fail;
	}
	for (size_t i = 0; i < opts->n_backends; i++) {
		st = sicha_backend_init(&c->backends[i],
			&opts->backends[i]);
		if (st != SICHA_OK) {
			goto fail;
		}
		c->n_backends++;
	}
	c->policy = opts->retry;
	if (c->policy.n_overrides > 0) {
		size_t bytes = c->policy.n_overrides *
			sizeof(*c->overrides);

		c->overrides = malloc(bytes);
		if (c->overrides == NULL) {
			st = SICHA_E_NOMEM;
			goto fail;
		}
		memcpy(c->overrides, c->policy.overrides, bytes);
		c->policy.overrides = c->overrides;
	}
	sicha_policy_resolve(&c->policy);

	c->transport = opts->transport;
#if defined(SICHA_WITH_CURL)
	if (c->transport == NULL) {
		c->curl_transport = sicha_curl_create();
		if (c->curl_transport == NULL) {
			st = SICHA_E_NOMEM;
			goto fail;
		}
		c->transport = c->curl_transport;
	}
#endif
	c->clock = sicha_clock_resolve(opts->clock);
	c->prng_seed = opts->prng_seed;
	if (c->prng_seed == 0) {
		/* environmental, NOT cryptographic: independent
		 * processes must not retry in lockstep */
		uint64_t mix = sicha_clock_real.now_ms(NULL);

		mix ^= (uint64_t)(uintptr_t)c * 0x9E3779B97F4A7C15ull;
		c->prng_seed = splitmix64(&mix);
		if (c->prng_seed == 0) {
			c->prng_seed = 1;
		}
	}
	c->max_response_bytes = opts->max_response_bytes != 0 ?
		opts->max_response_bytes : SICHA_DEFAULT_MAX_RESPONSE_BYTES;
	c->max_event_bytes = opts->max_event_bytes != 0 ?
		opts->max_event_bytes : SICHA_DEFAULT_MAX_EVENT_BYTES;
	{
		const char *ua = opts->user_agent;
		char buf[64];

		if (ua == NULL) {
			snprintf(buf, sizeof(buf), "sicha/%s",
				sicha_version_str());
			ua = buf;
		}
		c->user_agent = malloc(strlen(ua) + 1);
		if (c->user_agent == NULL) {
			st = SICHA_E_NOMEM;
			goto fail;
		}
		memcpy(c->user_agent, ua, strlen(ua) + 1);
	}
	{
		const char *dbg = getenv("SICHA_DEBUG");

		if (dbg != NULL && dbg[0] != '\0') {
			if (strcmp(dbg, "-") == 0 ||
				strcmp(dbg, "stderr") == 0) {
				c->debug = stderr;
			} else {
				c->debug = fopen(dbg, "a");
				c->debug_owned = c->debug != NULL;
			}
		}
	}
	*out = c;
	return SICHA_OK;

fail:
	sicha_client_destroy(c);
	return st;
}

void sicha_client_destroy(sicha_client *c)
{
	if (c == NULL) {
		return;
	}
#if defined(SICHA_WITH_CURL)
	sicha_curl_destroy(c->curl_transport);
#endif
	for (size_t i = 0; i < c->n_backends; i++) {
		sicha_backend_free(&c->backends[i]);
	}
	free(c->backends);
	free(c->overrides);
	free(c->user_agent);
	if (c->debug_owned && c->debug != NULL) {
		fclose(c->debug);
	}
	sicha_mutex_destroy(&c->debug_mu);
	free(c);
}

/* ------------------------------------------------------------------ */
/* One HTTP round trip                                                 */
/* ------------------------------------------------------------------ */

typedef struct attempt_io {
	sicha_result *r;
	int stream;
	sicha_stream_ctx sctx;
	sicha_sse sse;
	sicha_buf body;             /* non-stream accumulation          */
	int32_t http_status;
	char retry_after[64];
	int have_retry_after;
	int32_t sse_rc;
} attempt_io;

static int32_t attempt_on_status(void *ud, int32_t http_status,
	const sicha_header *headers, size_t n_headers)
{
	attempt_io *io = ud;

	io->http_status = http_status;
	for (size_t i = 0; i < n_headers; i++) {
		if (headers[i].name != NULL && headers[i].value != NULL &&
			sicha_header_name_eq(headers[i].name,
				"Retry-After")) {
			snprintf(io->retry_after, sizeof(io->retry_after),
				"%s", headers[i].value);
			io->have_retry_after = 1;
		}
	}
	return 0;
}

static int32_t attempt_on_body(void *ud, const char *bytes, size_t len)
{
	attempt_io *io = ud;

	if (io->stream) {
		int32_t rc = sicha_sse_feed(&io->sse, bytes, len,
			sicha_stream_on_event, &io->sctx);

		if (rc != 0) {
			io->sse_rc = rc;
			return 1;
		}
		return 0;
	}
	sicha_buf_append(&io->body, bytes, len);
	return sicha_buf_status(&io->body) == SICHA_OK ? 0 : 1;
}

/* Self-contained outcome of one round trip: everything the loop and
 * the record need survives the attempt io. */
typedef struct attempt_outcome {
	sicha_transport_status ts;
	int32_t http_status;
	int32_t body_state;
	uint64_t latency_ms;
	uint64_t retry_after_ms;    /* UINT64_MAX = none                */
	int delivered;              /* a callback got delta bytes       */
	sicha_status terminal;      /* SICHA_OK, or E_NOMEM             */
	char message[96];
	char excerpt[SICHA_EXCERPT_MAX];
	size_t excerpt_len;
} attempt_outcome;

static void outcome_message(attempt_outcome *o)
{
	if (o->terminal == SICHA_E_NOMEM) {
		snprintf(o->message, sizeof(o->message), "out of memory");
		return;
	}
	if (o->ts != SICHA_T_OK) {
		snprintf(o->message, sizeof(o->message), "transport: %s",
			sicha_transport_status_str(o->ts));
		return;
	}
	if (o->http_status < 200 || o->http_status > 299) {
		snprintf(o->message, sizeof(o->message), "http %d",
			(int)o->http_status);
		return;
	}
	switch (o->body_state) {
	case SICHA_BODY_EMPTY:
		snprintf(o->message, sizeof(o->message),
			"empty response body");
		break;
	case SICHA_BODY_MALFORMED:
		snprintf(o->message, sizeof(o->message),
			"malformed or incomplete response");
		break;
	case SICHA_BODY_TOOBIG:
		snprintf(o->message, sizeof(o->message),
			"response exceeds size cap");
		break;
	default:
		snprintf(o->message, sizeof(o->message), "ok");
		break;
	}
}

static void outcome_borrow_excerpt(attempt_outcome *o, const char *bytes,
	size_t len)
{
	if (bytes == NULL || len == 0) {
		return;
	}
	if (len > sizeof(o->excerpt)) {
		len = sizeof(o->excerpt);
	}
	memcpy(o->excerpt, bytes, len);
	o->excerpt_len = len;
}

static void run_attempt(sicha_client *c, const sicha_backend *b,
	const char *body, size_t body_len, const sicha_header *headers,
	size_t n_headers, const sicha_callbacks *cbs, sicha_cancel *cancel,
	sicha_result *r, int stream, int accumulate, attempt_outcome *o)
{
	attempt_io io;
	sicha_http_request rq;

	memset(o, 0, sizeof(*o));
	o->retry_after_ms = UINT64_MAX;

	memset(&io, 0, sizeof(io));
	io.r = r;
	io.stream = stream;
	sicha_buf_init(&io.body, c->max_response_bytes);
	if (stream) {
		sicha_sse_init(&io.sse, c->max_event_bytes);
		sicha_stream_ctx_init(&io.sctx, r, cbs, accumulate);
	}

	memset(&rq, 0, sizeof(rq));
	rq.struct_size = (uint32_t)sizeof(rq);
	rq.method = "POST";
	rq.url = b->url_chat;
	rq.headers = headers;
	rq.n_headers = n_headers;
	rq.body = body;
	rq.body_len = body_len;
	rq.timeouts = b->timeouts;
	debug_request(c, &rq);

	{
		sicha_http_sink sink = { &io, attempt_on_status,
			attempt_on_body };
		uint64_t t0 = c->clock->now_ms(c->clock->ud);

		o->ts = c->transport->perform(c->transport->ud, &rq, &sink,
			cancel, c->clock);
		o->latency_ms = c->clock->now_ms(c->clock->ud) - t0;
	}
	o->http_status = io.http_status;
	if (io.have_retry_after) {
		o->retry_after_ms = sicha_retry_after_ms(io.retry_after,
			SICHA_LEN_CSTR);
	}

	/* untangle sink-driven aborts */
	if (o->ts == SICHA_T_E_ABORTED_BY_SINK) {
		if (stream) {
			if (io.sctx.done) {
				o->ts = SICHA_T_OK; /* [DONE]: success  */
			} else if (io.sse_rc == SICHA_STREAM_CANCEL) {
				o->ts = SICHA_T_E_CANCELLED;
			} else if (io.sctx.err == SICHA_E_NOMEM ||
				sicha_sse_status(&io.sse) ==
					SICHA_E_NOMEM) {
				o->terminal = SICHA_E_NOMEM;
			} else {
				o->ts = SICHA_T_OK; /* size cap tripped */
				o->body_state = SICHA_BODY_TOOBIG;
			}
		} else {
			if (sicha_buf_status(&io.body) == SICHA_E_NOMEM) {
				o->terminal = SICHA_E_NOMEM;
			} else {
				o->ts = SICHA_T_OK;
				o->body_state = SICHA_BODY_TOOBIG;
			}
		}
	}
	if (o->ts == SICHA_T_E_NOMEM) {
		o->terminal = SICHA_E_NOMEM;
	}

	if (o->terminal == SICHA_OK && o->ts == SICHA_T_OK &&
		o->http_status >= 200 && o->http_status <= 299 &&
		o->body_state == SICHA_BODY_OK) {
		if (stream) {
			if (!io.sctx.done) {
				/* lenient EOF flush of a final event */
				int32_t rc = sicha_sse_finish(&io.sse,
					sicha_stream_on_event, &io.sctx);

				if (rc == SICHA_STREAM_CANCEL) {
					o->ts = SICHA_T_E_CANCELLED;
				} else if (rc == -1) {
					if (io.sctx.err == SICHA_E_NOMEM ||
						sicha_sse_status(&io.sse)
							== SICHA_E_NOMEM) {
						o->terminal =
							SICHA_E_NOMEM;
					} else {
						o->body_state =
							SICHA_BODY_TOOBIG;
					}
				}
			}
			if (o->terminal == SICHA_OK &&
				o->ts == SICHA_T_OK &&
				o->body_state == SICHA_BODY_OK &&
				!io.sctx.done && r->finish_raw == NULL) {
				/* neither [DONE] nor a finish_reason:
				 * incomplete or empty stream */
				o->body_state = io.sctx.saw_event ?
					SICHA_BODY_MALFORMED :
					SICHA_BODY_EMPTY;
			}
		} else {
			sicha_status st = sicha_response_parse(r,
				io.body.data != NULL ? io.body.data : "",
				io.body.len, &o->body_state);

			if (st != SICHA_OK) {
				o->terminal = st;
			} else {
				/* retain the verbatim body */
				size_t blen = 0;
				char *raw = sicha_buf_take(&io.body,
					&blen);

				if (raw == NULL) {
					o->terminal = SICHA_E_NOMEM;
				} else {
					free(r->raw_body);
					r->raw_body = raw;
					r->raw_body_len = blen;
				}
			}
		}
	}

	/* excerpt for response-shaped failure records */
	if (o->ts == SICHA_T_OK &&
		(o->http_status < 200 || o->http_status > 299 ||
			o->body_state != SICHA_BODY_OK)) {
		if (stream) {
			if (io.sctx.last_raw.len > 0) {
				outcome_borrow_excerpt(o,
					io.sctx.last_raw.data,
					io.sctx.last_raw.len);
			} else {
				outcome_borrow_excerpt(o, r->text.data,
					r->text.len);
			}
		} else if (r->raw_body != NULL && r->raw_body_len > 0) {
			/* 2xx parse path already moved the body into the
			 * result */
			outcome_borrow_excerpt(o, r->raw_body,
				r->raw_body_len);
		} else {
			outcome_borrow_excerpt(o, io.body.data,
				io.body.len);
		}
	}
	o->delivered = stream ? io.sctx.delivered : 0;
	outcome_message(o);

	sicha_buf_free(&io.body);
	if (stream) {
		sicha_sse_free(&io.sse);
		sicha_stream_ctx_free(&io.sctx);
	}
}

/* ------------------------------------------------------------------ */
/* Cancellation assist (KoboldCpp)                                     */
/* ------------------------------------------------------------------ */

static void kobold_abort_assist(sicha_client *c, const sicha_backend *b)
{
	sicha_http_request rq;
	sicha_header hdrs[2];
	size_t n = 0;
	char auth[512];

	if (b->url_abort == NULL) {
		return;
	}
	hdrs[n].name = "Content-Type";
	hdrs[n].value = "application/json";
	n++;
	if (b->api_key != NULL &&
		strlen(b->api_key) < sizeof(auth) - 8) {
		snprintf(auth, sizeof(auth), "Bearer %s", b->api_key);
		hdrs[n].name = "Authorization";
		hdrs[n].value = auth;
		n++;
	}
	memset(&rq, 0, sizeof(rq));
	rq.struct_size = (uint32_t)sizeof(rq);
	rq.method = "POST";
	rq.url = b->url_abort;
	rq.headers = hdrs;
	rq.n_headers = n;
	rq.body = "{}";
	rq.body_len = 2;
	rq.timeouts.connect_ms = 2000;
	rq.timeouts.first_byte_ms = 2000;
	rq.timeouts.idle_ms = 2000;
	rq.timeouts.total_ms = 2000;
	rq.flags = SICHA_HTTP_FIRE_AND_FORGET;
	debug_request(c, &rq);
	/* best effort: no sink, no cancel token, result ignored */
	(void)c->transport->perform(c->transport->ud, &rq, NULL, NULL,
		c->clock);
}

/* ------------------------------------------------------------------ */
/* The engine                                                          */
/* ------------------------------------------------------------------ */

static sicha_status chat_common(sicha_client *c,
	const sicha_request *req, const sicha_callbacks *cbs,
	sicha_cancel *cancel, sicha_result **out, int stream)
{
	sicha_result *r;
	sicha_status st;
	char **bodies = NULL;
	size_t *body_lens = NULL;
	sicha_header **headers = NULL;
	size_t *n_headers = NULL;
	uint64_t rng;
	uint32_t attempt_ordinal = 0;
	int any_delivered = 0;
	sicha_status final = SICHA_E_EXHAUSTED;

	if (out == NULL) {
		return SICHA_E_INVALID_ARG;
	}
	*out = NULL;
	if (c == NULL || req == NULL) {
		return SICHA_E_INVALID_ARG;
	}
	if (cbs != NULL && cbs->struct_size != (uint32_t)sizeof(*cbs)) {
		return SICHA_E_INVALID_ARG;
	}
	st = sicha_request_validate(req);
	if (st != SICHA_OK) {
		return st;
	}

	r = sicha_result_create(c->max_response_bytes);
	if (r == NULL) {
		return SICHA_E_NOMEM;
	}
	bodies = calloc(c->n_backends, sizeof(*bodies));
	body_lens = calloc(c->n_backends, sizeof(*body_lens));
	headers = calloc(c->n_backends, sizeof(*headers));
	n_headers = calloc(c->n_backends, sizeof(*n_headers));
	if (bodies == NULL || body_lens == NULL || headers == NULL ||
		n_headers == NULL) {
		final = SICHA_E_NOMEM;
		goto done;
	}
	{
		uint64_t seed = c->prng_seed ^
			(uint64_t)sicha_atomic_fetch_add_u32(
				&c->req_counter, 1);

		rng = seed;
	}

	for (size_t i = 0; i < c->n_backends; i++) {
		const sicha_backend *b = &c->backends[i];
		uint32_t retry_same_used = 0;
		uint32_t validation_used = 0;
		uint32_t try_of_backend = 0;

		for (;;) {
			attempt_outcome o;
			sicha_error_class cls;

			if (cancel != NULL &&
				sicha_cancel_is_cancelled(cancel)) {
				final = SICHA_E_CANCELLED;
				goto done;
			}
			/* lazily build the per-backend request pieces */
			if (bodies[i] == NULL) {
				st = sicha_build_chat_body(b, req, stream,
					&bodies[i], &body_lens[i]);
				if (st != SICHA_OK) {
					final = st;
					goto done;
				}
			}
			if (headers[i] == NULL) {
				st = sicha_build_headers(b, stream,
					c->user_agent, &headers[i],
					&n_headers[i]);
				if (st != SICHA_OK) {
					final = st;
					goto done;
				}
			}

			sicha_result_reset_content(r);
			r->backend = (int32_t)i;
			run_attempt(c, b, bodies[i], body_lens[i],
				headers[i], n_headers[i], cbs, cancel, r,
				stream,
				(req->flags & SICHA_REQ_NO_ACCUMULATE) == 0,
				&o);
			any_delivered = any_delivered || o.delivered;

			if (o.terminal != SICHA_OK) {
				/* record best-effort, then bail */
				cls = SICHA_CLASS_ADVANCE;
				final = o.terminal;
			} else {
				cls = sicha_classify(&c->policy, o.ts,
					o.http_status, o.body_state,
					r->finish);
			}

			/* validation runs on classified successes */
			if (o.terminal == SICHA_OK &&
				cls == SICHA_CLASS_NONE && cbs != NULL &&
				cbs->validate != NULL &&
				cbs->validate(cbs->ud, r) != 0) {
				cls = SICHA_CLASS_VALIDATION;
				snprintf(o.message, sizeof(o.message),
					"validator rejected");
				o.excerpt_len = 0;
				outcome_borrow_excerpt(&o, r->text.data,
					r->text.len);
			}

			/* exactly one record + one on_attempt per trip */
			{
				sicha_attempt a;

				memset(&a, 0, sizeof(a));
				a.struct_size = (uint32_t)sizeof(a);
				a.attempt = attempt_ordinal++;
				a.backend = (uint32_t)i;
				a.try_of_backend = try_of_backend++;
				a.model = b->model;
				a.error_class = cls;
				a.http_status = o.http_status;
				a.transport_status = o.ts;
				a.latency_ms = o.latency_ms;
				a.prompt_tokens = r->prompt_tokens;
				a.completion_tokens = r->completion_tokens;
				a.total_tokens = r->total_tokens;
				a.message = o.message;
				a.raw_body_excerpt = cls ==
						SICHA_CLASS_ADVANCE ||
					cls == SICHA_CLASS_VALIDATION ?
					o.excerpt : NULL;
				a.raw_body_excerpt_len =
					a.raw_body_excerpt != NULL ?
					o.excerpt_len : 0;
				if (sicha_result_add_attempt(r, &a) !=
					SICHA_OK) {
					final = SICHA_E_NOMEM;
					goto done;
				}
				debug_attempt(c,
					sicha_result_attempt(r,
						r->n_attempts - 1));
				if (cbs != NULL &&
					cbs->on_attempt != NULL) {
					cbs->on_attempt(cbs->ud,
						sicha_result_attempt(r,
							r->n_attempts - 1));
				}
			}

			if (final == SICHA_E_NOMEM) {
				goto done; /* terminal from run_attempt */
			}

			switch (cls) {
			case SICHA_CLASS_NONE:
				final = SICHA_OK;
				goto done;
			case SICHA_CLASS_ABORT:
				final = SICHA_E_ABORTED;
				goto done;
			case SICHA_CLASS_CANCELLED:
				if (b->url_abort != NULL) {
					kobold_abort_assist(c, b);
				}
				final = SICHA_E_CANCELLED;
				goto done;
			case SICHA_CLASS_VALIDATION:
				if (any_delivered && (c->policy.flags &
					SICHA_POLICY_RETRY_AFTER_DELTAS)
						== 0) {
					final = SICHA_E_STREAM_LOST;
					goto done;
				}
				if (validation_used <
					c->policy.validation_retries) {
					validation_used++;
					continue; /* re-roll, no backoff */
				}
				goto advance;
			case SICHA_CLASS_RETRY_SAME:
				if (any_delivered && (c->policy.flags &
					SICHA_POLICY_RETRY_AFTER_DELTAS)
						== 0) {
					final = SICHA_E_STREAM_LOST;
					goto done;
				}
				retry_same_used++;
				if (retry_same_used >=
					c->policy.max_tries) {
					goto advance;
				}
				{
					uint64_t wait;

					if (o.retry_after_ms !=
							UINT64_MAX &&
						c->policy.retry_after_cap_ms
							!= SICHA_DISABLED) {
						wait = o.retry_after_ms;
						if (wait > c->policy.
							retry_after_cap_ms) {
							wait = c->policy.
							retry_after_cap_ms;
						}
					} else {
						wait = sicha_backoff_ms(
							&c->policy,
							retry_same_used,
							splitmix64(&rng));
					}
					if (c->clock->wait_ms(c->clock->ud,
						cancel, wait) != 0) {
						final = SICHA_E_CANCELLED;
						goto done;
					}
				}
				continue;
			case SICHA_CLASS_ADVANCE:
			default:
				if (any_delivered && (c->policy.flags &
					SICHA_POLICY_RETRY_AFTER_DELTAS)
						== 0) {
					final = SICHA_E_STREAM_LOST;
					goto done;
				}
				goto advance;
			}
		}
advance:
		;
	}

done:
	if (bodies != NULL) {
		for (size_t i = 0; i < c->n_backends; i++) {
			free(bodies[i]);
		}
	}
	if (headers != NULL) {
		for (size_t i = 0; i < c->n_backends; i++) {
			sicha_headers_free(headers[i], n_headers[i]);
		}
	}
	free(bodies);
	free(body_lens);
	free(headers);
	free(n_headers);
	r->status = final;
	*out = r;
	return final;
}

sicha_status sicha_chat(sicha_client *c, const sicha_request *req,
	const sicha_callbacks *cbs, sicha_cancel *cancel,
	sicha_result **out)
{
	return chat_common(c, req, cbs, cancel, out, 0);
}

sicha_status sicha_chat_stream(sicha_client *c, const sicha_request *req,
	const sicha_callbacks *cbs, sicha_cancel *cancel,
	sicha_result **out)
{
	return chat_common(c, req, cbs, cancel, out, 1);
}
