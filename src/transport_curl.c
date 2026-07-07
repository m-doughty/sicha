/* The built-in libcurl transport.
 *
 * One easy handle driven by its own curl_multi, looped with
 * curl_multi_poll at <=100 ms ticks so cancellation and the
 * sicha-owned deadlines stay responsive.  Only the connect timeout is
 * delegated to curl (CURLOPT_CONNECTTIMEOUT_MS); first-byte, idle,
 * and total budgets are enforced in the loop — that keeps the four
 * classes unambiguous and makes first_byte = SICHA_INFINITE (local
 * engines prefilling for minutes) just work.
 *
 * HTTP/1.1 is FORCED: several OAI-compatible gateways hang on HTTP/2
 * header frames.  Redirects are off so classification stays honest.
 * TLS (certificate stores, proxies) is delegated entirely to libcurl
 * and its defaults.
 *
 * Handle pool: up to 8 {easy, multi} slots, mutex-guarded.  Reusing a
 * slot reuses its connection cache, so sequential requests to the
 * same host skip TCP/TLS setup.  Over-cap concurrency falls back to
 * transient handles.
 *
 * curl_global_init/cleanup are refcounted by libcurl itself; on
 * libcurls older than 7.84 the first init is not thread-safe, so
 * create the first client before spawning worker threads. */

#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "sicha_internal.h"
#include "sicha_thread.h"

#define CURL_POOL_CAP 8

typedef struct curl_slot {
	CURL *easy;
	CURLM *multi;
	int in_use;
} curl_slot;

typedef struct curl_state {
	sicha_transport vt; /* must stay first */
	sicha_mutex mu;
	curl_slot slots[CURL_POOL_CAP];
} curl_state;

/* ------------------------------------------------------------------ */
/* Per-transfer context                                                */
/* ------------------------------------------------------------------ */

typedef struct xfer {
	const sicha_http_sink *sink;
	const sicha_clock *clock;
	/* response header collection (until the blank line) */
	sicha_header headers[SICHA_MAX_HEADERS];
	size_t n_headers;
	size_t header_bytes;
	long http_status;
	int status_delivered;       /* on_status already fired          */
	int sink_aborted;           /* a sink callback returned nonzero */
	int caps_exceeded;
	int oom;
	uint64_t last_activity_ms;
} xfer;

static void xfer_free_headers(xfer *x)
{
	for (size_t i = 0; i < x->n_headers; i++) {
		free((char *)(uintptr_t)x->headers[i].name);
		free((char *)(uintptr_t)x->headers[i].value);
	}
	x->n_headers = 0;
	x->header_bytes = 0;
}

static char *dup_span(const char *s, size_t len)
{
	char *p = malloc(len + 1);

	if (p != NULL) {
		memcpy(p, s, len);
		p[len] = '\0';
	}
	return p;
}

/* Fires on_status exactly once, when the final (non-1xx) header block
 * completes. */
static int xfer_finish_headers(xfer *x)
{
	if (x->http_status >= 100 && x->http_status <= 199) {
		/* interim response: discard and wait for the real one */
		xfer_free_headers(x);
		return 0;
	}
	x->status_delivered = 1;
	if (x->sink != NULL && x->sink->on_status != NULL &&
		x->sink->on_status(x->sink->ud, (int32_t)x->http_status,
			x->headers, x->n_headers) != 0) {
		x->sink_aborted = 1;
		return 1;
	}
	return 0;
}

static size_t header_cb(char *buffer, size_t size, size_t nitems,
	void *ud)
{
	xfer *x = ud;
	size_t len = size * nitems;

	x->last_activity_ms = x->clock->now_ms(x->clock->ud);
	if (x->status_delivered) {
		/* trailers after the body: ignored */
		return len;
	}
	/* status line?  "HTTP/1.1 200 OK" — code after the first space */
	if (len >= 5 && memcmp(buffer, "HTTP/", 5) == 0) {
		const char *sp = memchr(buffer, ' ', len);
		long code = 0;

		if (sp != NULL) {
			for (const char *q = sp + 1;
				q < buffer + len && *q >= '0' &&
					*q <= '9'; q++) {
				code = code * 10 + (*q - '0');
			}
		}
		x->http_status = code;
		xfer_free_headers(x); /* fresh block (interim retry)     */
		return len;
	}
	/* end of a header block */
	if (len == 2 && buffer[0] == '\r' && buffer[1] == '\n') {
		return xfer_finish_headers(x) != 0 ? 0 : len;
	}
	if (len == 1 && buffer[0] == '\n') {
		return xfer_finish_headers(x) != 0 ? 0 : len;
	}
	/* a header field line: split at the first colon */
	{
		const char *colon = memchr(buffer, ':', len);
		size_t vstart;
		size_t vend = len;
		size_t nlen;

		if (colon == NULL) {
			return len; /* folded/garbage line: skip */
		}
		nlen = (size_t)(colon - buffer);
		vstart = nlen + 1;
		while (vstart < vend && (buffer[vstart] == ' ' ||
			buffer[vstart] == '\t')) {
			vstart++;
		}
		while (vend > vstart && (buffer[vend - 1] == '\r' ||
			buffer[vend - 1] == '\n' ||
			buffer[vend - 1] == ' ' ||
			buffer[vend - 1] == '\t')) {
			vend--;
		}
		if (x->n_headers >= SICHA_MAX_HEADERS ||
			len > SICHA_MAX_HEADER_BYTES ||
			x->header_bytes + len >
				SICHA_MAX_HEADERS_TOTAL_BYTES) {
			x->caps_exceeded = 1;
			return 0;
		}
		x->headers[x->n_headers].name = dup_span(buffer, nlen);
		x->headers[x->n_headers].value = dup_span(buffer + vstart,
			vend - vstart);
		if (x->headers[x->n_headers].name == NULL ||
			x->headers[x->n_headers].value == NULL) {
			free((char *)(uintptr_t)
				x->headers[x->n_headers].name);
			free((char *)(uintptr_t)
				x->headers[x->n_headers].value);
			x->oom = 1;
			return 0;
		}
		x->n_headers++;
		x->header_bytes += len;
	}
	return len;
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
	xfer *x = ud;
	size_t len = size * nmemb;

	x->last_activity_ms = x->clock->now_ms(x->clock->ud);
	if (x->sink != NULL && x->sink->on_body != NULL &&
		x->sink->on_body(x->sink->ud, ptr, len) != 0) {
		x->sink_aborted = 1;
		return 0; /* curl aborts with CURLE_WRITE_ERROR */
	}
	return len;
}

/* ------------------------------------------------------------------ */
/* CURLcode mapping                                                    */
/* ------------------------------------------------------------------ */

static sicha_transport_status map_curl_code(CURLcode rc, const xfer *x,
	uint64_t elapsed_ms, uint32_t connect_budget_ms)
{
	if (x->oom) {
		return SICHA_T_E_NOMEM;
	}
	if (x->caps_exceeded) {
		return SICHA_T_E_PROTOCOL;
	}
	if (x->sink_aborted) {
		return SICHA_T_E_ABORTED_BY_SINK;
	}
	switch (rc) {
	case CURLE_OK:
		return x->status_delivered ? SICHA_T_OK :
			SICHA_T_E_RESET; /* closed before any status */
	case CURLE_COULDNT_RESOLVE_HOST:
	case CURLE_COULDNT_RESOLVE_PROXY:
	case CURLE_COULDNT_CONNECT:
		return SICHA_T_E_CONNECT;
	case CURLE_OPERATION_TIMEDOUT:
		/* only the connect timeout is delegated to curl */
		return !x->status_delivered && connect_budget_ms !=
				SICHA_INFINITE &&
			elapsed_ms + 50 >= connect_budget_ms ?
			SICHA_T_E_TIMEOUT_CONNECT : SICHA_T_E_OTHER;
	case CURLE_PEER_FAILED_VERIFICATION:
	case CURLE_SSL_CONNECT_ERROR:
	case CURLE_SSL_CERTPROBLEM:
	case CURLE_SSL_CIPHER:
	case CURLE_SSL_CACERT_BADFILE:
	case CURLE_SSL_ISSUER_ERROR:
		return SICHA_T_E_TLS;
	case CURLE_SEND_ERROR:
	case CURLE_RECV_ERROR:
	case CURLE_PARTIAL_FILE:
	case CURLE_GOT_NOTHING:
	case CURLE_HTTP2:
	case CURLE_HTTP2_STREAM:
		return SICHA_T_E_RESET;
	case CURLE_WRITE_ERROR:
		/* our callbacks only fail for the reasons above, all
		 * already handled; anything else is curl-internal */
		return SICHA_T_E_OTHER;
	case CURLE_OUT_OF_MEMORY:
		return SICHA_T_E_NOMEM;
	default:
		return SICHA_T_E_OTHER;
	}
}

/* ------------------------------------------------------------------ */
/* Slot pool                                                           */
/* ------------------------------------------------------------------ */

static int slot_acquire(curl_state *s, curl_slot *out)
{
	sicha_mutex_lock(&s->mu);
	for (int i = 0; i < CURL_POOL_CAP; i++) {
		if (!s->slots[i].in_use && s->slots[i].easy != NULL) {
			s->slots[i].in_use = 1;
			*out = s->slots[i];
			sicha_mutex_unlock(&s->mu);
			return i;
		}
	}
	for (int i = 0; i < CURL_POOL_CAP; i++) {
		if (!s->slots[i].in_use && s->slots[i].easy == NULL) {
			s->slots[i].easy = curl_easy_init();
			s->slots[i].multi = curl_multi_init();
			if (s->slots[i].easy == NULL ||
				s->slots[i].multi == NULL) {
				if (s->slots[i].easy != NULL) {
					curl_easy_cleanup(s->slots[i].easy);
				}
				if (s->slots[i].multi != NULL) {
					curl_multi_cleanup(
						s->slots[i].multi);
				}
				s->slots[i].easy = NULL;
				s->slots[i].multi = NULL;
				sicha_mutex_unlock(&s->mu);
				return -2; /* NOMEM */
			}
			s->slots[i].in_use = 1;
			*out = s->slots[i];
			sicha_mutex_unlock(&s->mu);
			return i;
		}
	}
	sicha_mutex_unlock(&s->mu);
	/* pool saturated: transient handles */
	out->easy = curl_easy_init();
	out->multi = curl_multi_init();
	out->in_use = 1;
	if (out->easy == NULL || out->multi == NULL) {
		if (out->easy != NULL) {
			curl_easy_cleanup(out->easy);
		}
		if (out->multi != NULL) {
			curl_multi_cleanup(out->multi);
		}
		return -2;
	}
	return -1; /* transient */
}

static void slot_release(curl_state *s, int idx, curl_slot *slot)
{
	if (idx >= 0) {
		sicha_mutex_lock(&s->mu);
		s->slots[idx].in_use = 0;
		sicha_mutex_unlock(&s->mu);
		return;
	}
	curl_easy_cleanup(slot->easy);
	curl_multi_cleanup(slot->multi);
}

/* ------------------------------------------------------------------ */
/* perform                                                             */
/* ------------------------------------------------------------------ */

static sicha_transport_status curl_perform(void *ud,
	const sicha_http_request *req, const sicha_http_sink *sink,
	sicha_cancel *cancel, const sicha_clock *clock)
{
	curl_state *s = ud;
	curl_slot slot = { NULL, NULL, 0 };
	int slot_idx;
	xfer x;
	struct curl_slist *hdrs = NULL;
	sicha_transport_status result = SICHA_T_E_OTHER;
	uint64_t start;
	int removed = 0;

	if (req == NULL || req->struct_size != (uint32_t)sizeof(*req) ||
		req->url == NULL || clock == NULL) {
		return SICHA_T_E_OTHER;
	}
	slot_idx = slot_acquire(s, &slot);
	if (slot_idx == -2) {
		return SICHA_T_E_NOMEM;
	}

	memset(&x, 0, sizeof(x));
	x.sink = sink;
	x.clock = clock;

	curl_easy_reset(slot.easy);
	curl_easy_setopt(slot.easy, CURLOPT_URL, req->url);
	curl_easy_setopt(slot.easy, CURLOPT_HTTP_VERSION,
		CURL_HTTP_VERSION_1_1);
	curl_easy_setopt(slot.easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(slot.easy, CURLOPT_FOLLOWLOCATION, 0L);
	if (req->body != NULL) {
		curl_easy_setopt(slot.easy, CURLOPT_POST, 1L);
		curl_easy_setopt(slot.easy, CURLOPT_POSTFIELDS, req->body);
		curl_easy_setopt(slot.easy, CURLOPT_POSTFIELDSIZE_LARGE,
			(curl_off_t)req->body_len);
	} else if (req->method != NULL &&
		strcmp(req->method, "GET") == 0) {
		curl_easy_setopt(slot.easy, CURLOPT_HTTPGET, 1L);
	} else {
		curl_easy_setopt(slot.easy, CURLOPT_CUSTOMREQUEST,
			req->method != NULL ? req->method : "POST");
	}
	if (req->timeouts.connect_ms != SICHA_INFINITE) {
		curl_easy_setopt(slot.easy, CURLOPT_CONNECTTIMEOUT_MS,
			(long)req->timeouts.connect_ms);
	}
	for (size_t i = 0; i < req->n_headers; i++) {
		size_t nlen = strlen(req->headers[i].name);
		size_t vlen = strlen(req->headers[i].value);
		char *line = malloc(nlen + vlen + 3);
		struct curl_slist *next;

		if (line == NULL) {
			result = SICHA_T_E_NOMEM;
			goto out_headers;
		}
		memcpy(line, req->headers[i].name, nlen);
		line[nlen] = ':';
		line[nlen + 1] = ' ';
		memcpy(line + nlen + 2, req->headers[i].value, vlen + 1);
		next = curl_slist_append(hdrs, line);
		free(line);
		if (next == NULL) {
			result = SICHA_T_E_NOMEM;
			goto out_headers;
		}
		hdrs = next;
	}
	{
		/* suppress Expect: 100-continue delays on POSTs */
		struct curl_slist *next = curl_slist_append(hdrs,
			"Expect:");

		if (next == NULL) {
			result = SICHA_T_E_NOMEM;
			goto out_headers;
		}
		hdrs = next;
	}
	curl_easy_setopt(slot.easy, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(slot.easy, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(slot.easy, CURLOPT_HEADERDATA, &x);
	curl_easy_setopt(slot.easy, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(slot.easy, CURLOPT_WRITEDATA, &x);

	if (curl_multi_add_handle(slot.multi, slot.easy) != CURLM_OK) {
		result = SICHA_T_E_OTHER;
		goto out_headers;
	}

	start = clock->now_ms(clock->ud);
	x.last_activity_ms = start;
	for (;;) {
		int running = 0;
		CURLMcode mrc = curl_multi_perform(slot.multi, &running);

		if (mrc != CURLM_OK) {
			result = mrc == CURLM_OUT_OF_MEMORY ?
				SICHA_T_E_NOMEM : SICHA_T_E_OTHER;
			break;
		}
		/* finished? */
		{
			int msgs = 0;
			CURLMsg *msg = curl_multi_info_read(slot.multi,
				&msgs);

			if (msg != NULL && msg->msg == CURLMSG_DONE) {
				uint64_t elapsed =
					clock->now_ms(clock->ud) - start;

				result = map_curl_code(
					msg->data.result, &x, elapsed,
					req->timeouts.connect_ms);
				break;
			}
		}
		if (running == 0) {
			/* no message yet but nothing running: settle on
			 * the next info_read pass */
			continue;
		}
		/* sicha-owned deadlines */
		{
			uint64_t now = clock->now_ms(clock->ud);

			if (cancel != NULL &&
				sicha_cancel_is_cancelled(cancel)) {
				result = SICHA_T_E_CANCELLED;
				break;
			}
			if (x.oom) {
				result = SICHA_T_E_NOMEM;
				break;
			}
			if (x.caps_exceeded) {
				result = SICHA_T_E_PROTOCOL;
				break;
			}
			if (x.sink_aborted) {
				result = SICHA_T_E_ABORTED_BY_SINK;
				break;
			}
			if (req->timeouts.total_ms != SICHA_INFINITE &&
				now - start >= req->timeouts.total_ms) {
				result = SICHA_T_E_TIMEOUT_TOTAL;
				break;
			}
			if (!x.status_delivered &&
				req->timeouts.first_byte_ms !=
					SICHA_INFINITE &&
				now - start >=
					req->timeouts.first_byte_ms) {
				result = SICHA_T_E_TIMEOUT_FIRST_BYTE;
				break;
			}
			if (x.status_delivered &&
				req->timeouts.idle_ms != SICHA_INFINITE &&
				now - x.last_activity_ms >=
					req->timeouts.idle_ms) {
				result = SICHA_T_E_TIMEOUT_IDLE;
				break;
			}
		}
		{
			int numfds = 0;

#if defined(CURL_AT_LEAST_VERSION) && CURL_AT_LEAST_VERSION(7, 66, 0)
			curl_multi_poll(slot.multi, NULL, 0, 100,
				&numfds);
#else
			curl_multi_wait(slot.multi, NULL, 0, 100,
				&numfds);
#endif
		}
	}
	curl_multi_remove_handle(slot.multi, slot.easy);
	removed = 1;

out_headers:
	if (!removed) {
		/* handle was never added or the add failed */
		curl_multi_remove_handle(slot.multi, slot.easy);
	}
	curl_slist_free_all(hdrs);
	xfer_free_headers(&x);
	slot_release(s, slot_idx, &slot);
	return result;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

sicha_transport *sicha_curl_create(void)
{
	curl_state *s;

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
		return NULL;
	}
	s = calloc(1, sizeof(*s));
	if (s == NULL) {
		curl_global_cleanup();
		return NULL;
	}
	s->vt.struct_size = (uint32_t)sizeof(s->vt);
	s->vt.ud = s;
	s->vt.perform = curl_perform;
	sicha_mutex_init(&s->mu);
	return &s->vt;
}

void sicha_curl_destroy(sicha_transport *t)
{
	curl_state *s;

	if (t == NULL) {
		return;
	}
	s = (curl_state *)t;
	for (int i = 0; i < CURL_POOL_CAP; i++) {
		if (s->slots[i].easy != NULL) {
			curl_easy_cleanup(s->slots[i].easy);
		}
		if (s->slots[i].multi != NULL) {
			curl_multi_cleanup(s->slots[i].multi);
		}
	}
	sicha_mutex_destroy(&s->mu);
	free(s);
	curl_global_cleanup();
}
