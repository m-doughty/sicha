/* Shared builders for the engine suites (backoff / routing / cancel /
 * determinism / threads).  Header-only static helpers so the public
 * smoke test never links whitebox internals through the support
 * library. */

#ifndef SICHA_TEST_ENGINE_HELPERS_H
#define SICHA_TEST_ENGINE_HELPERS_H

#include "sicha_internal.h"
#include "support.h"

/* Canned wire payloads (compile-time concatenation). */
#define T_OK_BODY(content) \
	"{\"model\":\"served\",\"choices\":[{\"message\":{\"content\":\"" \
	content "\"},\"finish_reason\":\"stop\"}],\"usage\":" \
	"{\"prompt_tokens\":5,\"completion_tokens\":2,\"total_tokens\":7}}"

#define T_SSE_BODY(a, b) \
	"data: {\"model\":\"served\",\"choices\":[{\"delta\":" \
	"{\"content\":\"" a "\"}}]}\n\n" \
	"data: {\"choices\":[{\"delta\":{\"content\":\"" b "\"}," \
	"\"finish_reason\":\"stop\"}]}\n\n" \
	"data: [DONE]\n\n"

static const sicha_message T_MSG_HI = { SICHA_ROLE_USER, "hi",
	SICHA_LEN_CSTR, NULL };

static inline sicha_request t_req1(void)
{
	sicha_request r;

	memset(&r, 0, sizeof(r));
	r.struct_size = (uint32_t)sizeof(r);
	r.messages = &T_MSG_HI;
	r.n_messages = 1;
	return r;
}

static inline sicha_backend_desc t_backend(const char *url, const char *model)
{
	sicha_backend_desc d;

	memset(&d, 0, sizeof(d));
	d.struct_size = (uint32_t)sizeof(d);
	d.base_url = url;
	d.model = model;
	return d;
}

/* A full engine environment: script transport + fake clock + client. */
typedef struct t_env {
	sicha_transport *script;
	t_fake_clock fc;
	sicha_client *client;
} t_env;

/* Build a client over `n` backends (base urls http://b<i>.local/v1,
 * models m0..m<n-1>).  policy may be NULL (defaults).  Fixed seed 42
 * unless overridden with t_env_init_seeded. */
static inline int t_env_init_ex(t_env *e, size_t n,
	const sicha_retry_policy *policy, uint64_t seed, uint32_t flags0)
{
	static const char *urls[] = { "http://b0.local/v1",
		"http://b1.local/v1", "http://b2.local/v1" };
	static const char *models[] = { "m0", "m1", "m2" };
	sicha_backend_desc descs[3];
	sicha_client_opts opts;

	if (n > 3) {
		return 0;
	}
	memset(e, 0, sizeof(*e));
	e->script = sicha_script_create();
	if (e->script == NULL) {
		return 0;
	}
	t_fake_clock_init(&e->fc, 0);
	for (size_t i = 0; i < n; i++) {
		descs[i] = t_backend(urls[i], models[i]);
		if (i == 0) {
			descs[i].flags = flags0;
		}
	}
	memset(&opts, 0, sizeof(opts));
	opts.struct_size = (uint32_t)sizeof(opts);
	opts.backends = descs;
	opts.n_backends = n;
	if (policy != NULL) {
		opts.retry = *policy;
	}
	opts.transport = e->script;
	opts.clock = &e->fc.clock;
	opts.prng_seed = seed;
	if (sicha_client_create(&opts, &e->client) != SICHA_OK) {
		sicha_script_destroy(e->script);
		return 0;
	}
	return 1;
}

static inline int t_env_init(t_env *e, size_t n, const sicha_retry_policy *policy)
{
	return t_env_init_ex(e, n, policy, 42, 0);
}

static inline void t_env_free(t_env *e)
{
	sicha_client_destroy(e->client);
	sicha_script_destroy(e->script);
}

/* Queue helpers. */
static inline void t_push_http(sicha_transport *t, int32_t http_status,
	const char *body, const sicha_header *headers, size_t n_headers)
{
	sicha_script_response r;

	memset(&r, 0, sizeof(r));
	r.struct_size = (uint32_t)sizeof(r);
	r.status = SICHA_T_OK;
	r.http_status = http_status;
	r.body = body;
	r.body_len = SICHA_LEN_CSTR;
	r.headers = headers;
	r.n_headers = n_headers;
	T_CHECK(sicha_script_push(t, &r) == SICHA_OK);
}

static inline void t_push_ok(sicha_transport *t, const char *body)
{
	t_push_http(t, 200, body, NULL, 0);
}

static inline void t_push_terr(sicha_transport *t, sicha_transport_status ts)
{
	sicha_script_response r;

	memset(&r, 0, sizeof(r));
	r.struct_size = (uint32_t)sizeof(r);
	r.status = ts;
	T_CHECK(sicha_script_push(t, &r) == SICHA_OK);
}

/* HTTP 200 that dies (transport error) after `after` body bytes. */
static inline void t_push_broken_body(sicha_transport *t, const char *body,
	size_t after, sicha_transport_status ts)
{
	sicha_script_response r;

	memset(&r, 0, sizeof(r));
	r.struct_size = (uint32_t)sizeof(r);
	r.status = ts;
	r.http_status = 200;
	r.body = body;
	r.body_len = SICHA_LEN_CSTR;
	r.fail_after_bytes = after;
	T_CHECK(sicha_script_push(t, &r) == SICHA_OK);
}

/* The engine's per-request jitter stream, replicated for exactness
 * assertions: stream k of client seed s is splitmix64 over
 * s ^ request_ordinal. */
static inline uint64_t t_jitter_stream(uint64_t seed, uint32_t request_ordinal,
	int nth)
{
	t_rng rng = { seed ^ request_ordinal };
	uint64_t v = 0;

	for (int i = 0; i <= nth; i++) {
		v = t_rng_next(&rng);
	}
	return v;
}

#endif /* SICHA_TEST_ENGINE_HELPERS_H */
