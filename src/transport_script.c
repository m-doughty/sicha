/* The public scripted transport: FIFO-scripted responses, full call
 * recording, and virtual delays consumed through the CLIENT's clock —
 * under a fake clock every timeout class reproduces deterministically
 * and instantly.  The C analogue of LLM::Chat::Backend::Mock.
 *
 * Deadline semantics (mirrors what the curl transport enforces):
 *   - connect_ms and total_ms bound the connect phase;
 *   - first_byte_ms and total_ms bound the wait for the status line;
 *   - idle_ms (per chunk) and total_ms bound the body;
 *   - the phase-specific timeout wins when both cross in one wait.
 * Cancellation is polled at <= 100 ms wait granularity. */

#include <stdlib.h>
#include <string.h>

#include "sicha_internal.h"
#include "sicha_thread.h"

typedef struct script_call {
	char *url;
	char *body;
	size_t body_len;
	sicha_header *headers;
	size_t n_headers;
} script_call;

typedef struct script_state {
	sicha_transport vt; /* must stay first: transport* casts back */
	sicha_mutex mu;
	sicha_script_response *queue; /* deep copies, owned */
	size_t q_len;
	size_t q_cap;
	script_call *calls;
	size_t c_len;
	size_t c_cap;
} script_state;

static char *dup_bytes_n(const char *s, size_t len)
{
	char *p = malloc(len + 1);

	if (p != NULL) {
		if (len > 0) {
			memcpy(p, s, len);
		}
		p[len] = '\0';
	}
	return p;
}

static void headers_deep_free(sicha_header *h, size_t n)
{
	if (h == NULL) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		free((char *)(uintptr_t)h[i].name);
		free((char *)(uintptr_t)h[i].value);
	}
	free(h);
}

static sicha_header *headers_deep_copy(const sicha_header *src, size_t n)
{
	sicha_header *h;

	if (n == 0) {
		return calloc(1, sizeof(*h)); /* non-NULL sentinel */
	}
	h = calloc(n, sizeof(*h));
	if (h == NULL) {
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		h[i].name = src[i].name != NULL ?
			dup_bytes_n(src[i].name, strlen(src[i].name)) :
			dup_bytes_n("", 0);
		h[i].value = src[i].value != NULL ?
			dup_bytes_n(src[i].value, strlen(src[i].value)) :
			dup_bytes_n("", 0);
		if (h[i].name == NULL || h[i].value == NULL) {
			headers_deep_free(h, n);
			return NULL;
		}
	}
	return h;
}

static void response_free_owned(sicha_script_response *r)
{
	free((char *)(uintptr_t)r->body);
	headers_deep_free((sicha_header *)(uintptr_t)r->headers,
		r->n_headers);
	memset(r, 0, sizeof(*r));
}

/* ------------------------------------------------------------------ */
/* Deadline-aware virtual waiting                                      */
/* ------------------------------------------------------------------ */

static uint64_t abs_deadline(uint64_t start, uint32_t budget_ms)
{
	if (budget_ms == SICHA_INFINITE) {
		return UINT64_MAX;
	}
	return start > UINT64_MAX - budget_ms ? UINT64_MAX :
		start + budget_ms;
}

/* Wait out `delay_ms` of virtual time; if deadline a (then b) is
 * crossed first, return its status.  SICHA_T_OK when the delay
 * completes. */
static sicha_transport_status delay_within(const sicha_clock *ck,
	sicha_cancel *cancel, uint64_t delay_ms, uint64_t dl_a,
	sicha_transport_status st_a, uint64_t dl_b,
	sicha_transport_status st_b)
{
	uint64_t start = ck->now_ms(ck->ud);
	uint64_t target = start > UINT64_MAX - delay_ms ? UINT64_MAX :
		start + delay_ms;

	for (;;) {
		uint64_t now = ck->now_ms(ck->ud);
		uint64_t until;
		uint64_t chunk;

		if (cancel != NULL && sicha_cancel_is_cancelled(cancel)) {
			return SICHA_T_E_CANCELLED;
		}
		if (now >= dl_a) {
			return st_a;
		}
		if (now >= dl_b) {
			return st_b;
		}
		if (now >= target) {
			return SICHA_T_OK;
		}
		until = target;
		if (dl_a < until) {
			until = dl_a;
		}
		if (dl_b < until) {
			until = dl_b;
		}
		chunk = until - now;
		if (chunk > 100) {
			chunk = 100;
		}
		if (ck->wait_ms(ck->ud, cancel, chunk) != 0) {
			return SICHA_T_E_CANCELLED;
		}
	}
}

/* ------------------------------------------------------------------ */
/* perform                                                             */
/* ------------------------------------------------------------------ */

static sicha_transport_status script_perform(void *ud,
	const sicha_http_request *req, const sicha_http_sink *sink,
	sicha_cancel *cancel, const sicha_clock *clock)
{
	script_state *s = ud;
	sicha_script_response resp = { 0 };
	int have_resp = 0;
	uint64_t start;
	uint64_t dl_total;
	sicha_transport_status st;

	if (req == NULL || req->struct_size != (uint32_t)sizeof(*req) ||
		clock == NULL) {
		return SICHA_T_E_OTHER;
	}

	/* record the call and pop the next scripted response */
	sicha_mutex_lock(&s->mu);
	if (s->c_len == s->c_cap) {
		size_t cap = s->c_cap == 0 ? 8 : s->c_cap * 2;
		script_call *p = realloc(s->calls, cap * sizeof(*p));

		if (p == NULL) {
			sicha_mutex_unlock(&s->mu);
			return SICHA_T_E_NOMEM;
		}
		s->calls = p;
		s->c_cap = cap;
	}
	{
		script_call *c = &s->calls[s->c_len];

		c->url = dup_bytes_n(req->url != NULL ? req->url : "",
			req->url != NULL ? strlen(req->url) : 0);
		c->body = dup_bytes_n(req->body != NULL ? req->body : "",
			req->body != NULL ? req->body_len : 0);
		c->body_len = req->body != NULL ? req->body_len : 0;
		c->headers = headers_deep_copy(req->headers,
			req->n_headers);
		c->n_headers = req->n_headers;
		if (c->url == NULL || c->body == NULL ||
			c->headers == NULL) {
			free(c->url);
			free(c->body);
			headers_deep_free(c->headers, c->n_headers);
			sicha_mutex_unlock(&s->mu);
			return SICHA_T_E_NOMEM;
		}
		s->c_len++;
	}
	if (s->q_len > 0) {
		resp = s->queue[0]; /* take ownership of the copies */
		memmove(&s->queue[0], &s->queue[1],
			(s->q_len - 1) * sizeof(s->queue[0]));
		s->q_len--;
		have_resp = 1;
	}
	sicha_mutex_unlock(&s->mu);

	if (!have_resp) {
		return SICHA_T_E_CONNECT;
	}

	start = clock->now_ms(clock->ud);
	dl_total = abs_deadline(start, req->timeouts.total_ms);

	/* connect phase */
	st = delay_within(clock, cancel, resp.connect_delay_ms,
		abs_deadline(start, req->timeouts.connect_ms),
		SICHA_T_E_TIMEOUT_CONNECT, dl_total,
		SICHA_T_E_TIMEOUT_TOTAL);
	if (st != SICHA_T_OK) {
		goto out;
	}
	/* pure transport failure before any HTTP */
	if (resp.status != SICHA_T_OK && resp.fail_after_bytes == 0) {
		st = resp.status;
		goto out;
	}
	/* waiting for the status line */
	st = delay_within(clock, cancel, resp.first_byte_delay_ms,
		abs_deadline(start, req->timeouts.first_byte_ms),
		SICHA_T_E_TIMEOUT_FIRST_BYTE, dl_total,
		SICHA_T_E_TIMEOUT_TOTAL);
	if (st != SICHA_T_OK) {
		goto out;
	}
	if (sink != NULL && sink->on_status != NULL &&
		sink->on_status(sink->ud, resp.http_status, resp.headers,
			resp.n_headers) != 0) {
		st = SICHA_T_E_ABORTED_BY_SINK;
		goto out;
	}
	/* body */
	{
		size_t remaining = resp.body_len;
		size_t pos = 0;
		size_t budget = resp.status != SICHA_T_OK ?
			resp.fail_after_bytes : resp.body_len;

		if (budget < remaining) {
			remaining = budget;
		}
		while (remaining > 0) {
			size_t n = resp.chunk_size == 0 ? remaining :
				resp.chunk_size;
			uint64_t chunk_start;

			if (n > remaining) {
				n = remaining;
			}
			chunk_start = clock->now_ms(clock->ud);
			st = delay_within(clock, cancel,
				resp.per_chunk_delay_ms,
				abs_deadline(chunk_start,
					req->timeouts.idle_ms),
				SICHA_T_E_TIMEOUT_IDLE, dl_total,
				SICHA_T_E_TIMEOUT_TOTAL);
			if (st != SICHA_T_OK) {
				goto out;
			}
			if (sink != NULL && sink->on_body != NULL &&
				sink->on_body(sink->ud, resp.body + pos,
					n) != 0) {
				st = SICHA_T_E_ABORTED_BY_SINK;
				goto out;
			}
			pos += n;
			remaining -= n;
		}
	}
	st = resp.status; /* OK, or the scripted failure after bytes */
out:
	response_free_owned(&resp);
	return st;
}

/* ------------------------------------------------------------------ */
/* Public surface                                                      */
/* ------------------------------------------------------------------ */

sicha_transport *sicha_script_create(void)
{
	script_state *s = calloc(1, sizeof(*s));

	if (s == NULL) {
		return NULL;
	}
	s->vt.struct_size = (uint32_t)sizeof(s->vt);
	s->vt.ud = s;
	s->vt.perform = script_perform;
	sicha_mutex_init(&s->mu);
	return &s->vt;
}

void sicha_script_destroy(sicha_transport *t)
{
	script_state *s;

	if (t == NULL) {
		return;
	}
	s = (script_state *)t;
	for (size_t i = 0; i < s->q_len; i++) {
		response_free_owned(&s->queue[i]);
	}
	free(s->queue);
	for (size_t i = 0; i < s->c_len; i++) {
		free(s->calls[i].url);
		free(s->calls[i].body);
		headers_deep_free(s->calls[i].headers,
			s->calls[i].n_headers);
	}
	free(s->calls);
	sicha_mutex_destroy(&s->mu);
	free(s);
}

sicha_status sicha_script_push(sicha_transport *t,
	const sicha_script_response *r)
{
	script_state *s = (script_state *)t;
	sicha_script_response copy;
	size_t total_hdr_bytes = 0;

	if (t == NULL || r == NULL ||
		r->struct_size != (uint32_t)sizeof(*r)) {
		return SICHA_E_INVALID_ARG;
	}
	if (r->n_headers > SICHA_MAX_HEADERS ||
		(r->n_headers > 0 && r->headers == NULL)) {
		return SICHA_E_INVALID_ARG;
	}
	for (size_t i = 0; i < r->n_headers; i++) {
		size_t nb = (r->headers[i].name != NULL ?
			strlen(r->headers[i].name) : 0) +
			(r->headers[i].value != NULL ?
				strlen(r->headers[i].value) : 0);

		if (nb > SICHA_MAX_HEADER_BYTES) {
			return SICHA_E_INVALID_ARG;
		}
		total_hdr_bytes += nb;
	}
	if (total_hdr_bytes > SICHA_MAX_HEADERS_TOTAL_BYTES) {
		return SICHA_E_INVALID_ARG;
	}

	copy = *r;
	copy.body_len = r->body == NULL ? 0 :
		(r->body_len == SICHA_LEN_CSTR ? strlen(r->body) :
			r->body_len);
	copy.body = dup_bytes_n(r->body != NULL ? r->body : "",
		copy.body_len);
	copy.headers = headers_deep_copy(r->headers, r->n_headers);
	if (copy.body == NULL || copy.headers == NULL) {
		free((char *)(uintptr_t)copy.body);
		headers_deep_free((sicha_header *)(uintptr_t)copy.headers,
			copy.n_headers);
		return SICHA_E_NOMEM;
	}

	sicha_mutex_lock(&s->mu);
	if (s->q_len == s->q_cap) {
		size_t cap = s->q_cap == 0 ? 8 : s->q_cap * 2;
		sicha_script_response *p = realloc(s->queue,
			cap * sizeof(*p));

		if (p == NULL) {
			sicha_mutex_unlock(&s->mu);
			response_free_owned(&copy);
			return SICHA_E_NOMEM;
		}
		s->queue = p;
		s->q_cap = cap;
	}
	s->queue[s->q_len++] = copy;
	sicha_mutex_unlock(&s->mu);
	return SICHA_OK;
}

uint32_t sicha_script_call_count(const sicha_transport *t)
{
	script_state *s = (script_state *)(uintptr_t)t;
	uint32_t n;

	if (t == NULL) {
		return 0;
	}
	sicha_mutex_lock(&s->mu);
	n = (uint32_t)s->c_len;
	sicha_mutex_unlock(&s->mu);
	return n;
}

/* The calls ARRAY may be reallocated by a concurrent perform, so the
 * entry is snapshotted by value under the lock.  The string and
 * header allocations it points at are stable until destroy (which
 * must not race reads — documented in the header). */
static script_call call_snapshot(const sicha_transport *t, uint32_t i)
{
	script_state *s = (script_state *)(uintptr_t)t;
	script_call c = { NULL, NULL, 0, NULL, 0 };

	if (t == NULL) {
		return c;
	}
	sicha_mutex_lock(&s->mu);
	if (i < s->c_len) {
		c = s->calls[i];
	}
	sicha_mutex_unlock(&s->mu);
	return c;
}

const char *sicha_script_call_url(const sicha_transport *t, uint32_t i)
{
	return call_snapshot(t, i).url;
}

const char *sicha_script_call_body(const sicha_transport *t, uint32_t i,
	size_t *len)
{
	script_call c = call_snapshot(t, i);

	if (len != NULL) {
		*len = c.body_len;
	}
	return c.body;
}

const char *sicha_script_call_header(const sicha_transport *t,
	uint32_t i, const char *name)
{
	script_call c = call_snapshot(t, i);

	if (c.headers == NULL || name == NULL) {
		return NULL;
	}
	for (size_t k = 0; k < c.n_headers; k++) {
		if (sicha_header_name_eq(c.headers[k].name, name)) {
			return c.headers[k].value;
		}
	}
	return NULL;
}
