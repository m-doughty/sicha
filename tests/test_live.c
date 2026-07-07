/* OPT-IN live-endpoint integration test: runs the full client (built-
 * in curl transport, real clock) against a real OAI-compatible
 * endpoint.  Configure with environment variables:
 *
 *   SICHA_LIVE_BASE_URL       e.g. http://localhost:5001/v1   (required)
 *   SICHA_LIVE_MODEL          model id                        (required)
 *   SICHA_LIVE_API_KEY        bearer token                    (optional)
 *   SICHA_LIVE_KOBOLD_ASSIST  "1" = set the KoboldCpp abort-assist
 *                             flag so cancellation also exercises
 *                             POST /api/extra/abort             (optional)
 *
 * Unconfigured, it exits 77 — ctest reports it as ***Skipped (wired
 * via SKIP_RETURN_CODE), never as a silent pass.
 *
 * Cost discipline: three requests, tiny max_tokens, temperature 0.
 * The third request asks for a long generation but cancels after a
 * few deltas, so it bills only a handful of tokens on token-metered
 * providers.
 *
 * Public API only — this doubles as an FFI-shaped consumer of the
 * real library target. */

#include <stdlib.h>

#include "support.h"

#define LIVE_SKIP 77

typedef struct live_cfg {
	const char *base_url;
	const char *model;
	const char *api_key;
	int kobold_assist;
} live_cfg;

static sicha_client *live_client(const live_cfg *cfg)
{
	sicha_backend_desc d;
	sicha_client_opts opts;
	sicha_client *client = NULL;

	memset(&d, 0, sizeof(d));
	d.struct_size = (uint32_t)sizeof(d);
	d.base_url = cfg->base_url;
	d.model = cfg->model;
	d.api_key = cfg->api_key;
	if (cfg->kobold_assist) {
		d.flags |= SICHA_BACKEND_KOBOLD_CANCEL_ASSIST;
	}
	/* local engines can prefill for a long time before the first
	 * header; keep the wall-clock bounded overall */
	d.timeouts.connect_ms = 15000;
	d.timeouts.first_byte_ms = SICHA_INFINITE;
	d.timeouts.idle_ms = 120000;
	d.timeouts.total_ms = 600000;
	memset(&opts, 0, sizeof(opts));
	opts.struct_size = (uint32_t)sizeof(opts);
	opts.backends = &d;
	opts.n_backends = 1;
	T_CHECK(sicha_client_create(&opts, &client) == SICHA_OK);
	return client;
}

static sicha_request live_request(const sicha_message *msgs, size_t n,
	int32_t max_tokens)
{
	sicha_request r;

	memset(&r, 0, sizeof(r));
	r.struct_size = (uint32_t)sizeof(r);
	r.messages = msgs;
	r.n_messages = n;
	r.set_mask = SICHA_SET_MAX_TOKENS | SICHA_SET_TEMPERATURE;
	r.max_tokens = max_tokens;
	r.temperature = 0.0;
	return r;
}

static void report_attempts(const sicha_result *r)
{
	for (uint32_t i = 0; i < sicha_result_attempt_count(r); i++) {
		const sicha_attempt *a = sicha_result_attempt(r, i);

		fprintf(stderr,
			"  attempt %u: class=%s transport=%s http=%d "
			"latency=%ums: %s\n",
			a->attempt, sicha_error_class_str(a->error_class),
			sicha_transport_status_str(a->transport_status),
			(int)a->http_status, (unsigned)a->latency_ms,
			a->message);
		if (a->raw_body_excerpt_len > 0) {
			fprintf(stderr, "    body: %.*s\n",
				(int)a->raw_body_excerpt_len,
				a->raw_body_excerpt);
		}
	}
}

static void check_live_blocking(const live_cfg *cfg)
{
	sicha_client *client = live_client(cfg);
	sicha_message msgs[2] = {
		{ SICHA_ROLE_SYSTEM,
			"You answer with one short sentence.",
			SICHA_LEN_CSTR, NULL },
		{ SICHA_ROLE_USER, "Say hello.", SICHA_LEN_CSTR, NULL },
	};
	sicha_request req = live_request(msgs, 2, 48);
	sicha_result *r = NULL;
	sicha_status st = sicha_chat(client, &req, NULL, NULL, &r);

	T_CHECK(st == SICHA_OK);
	if (st != SICHA_OK) {
		report_attempts(r);
	} else {
		size_t len = 0;
		const char *text = sicha_result_text(r, &len);
		const sicha_attempt *a = sicha_result_attempt(r,
			sicha_result_attempt_count(r) - 1);

		T_CHECK(len > 0);
		T_CHECK(a != NULL &&
			a->error_class == SICHA_CLASS_NONE);
		T_CHECK(a != NULL && a->http_status >= 200 &&
			a->http_status <= 299);
		/* the verbatim body is retained on non-streaming calls */
		{
			size_t blen = 0;

			sicha_result_raw_body(r, &blen);
			T_CHECK(blen > 0);
		}
		fprintf(stderr, "live blocking: model=\"%s\" finish=%s "
			"tokens=%lld text=%.*s\n",
			sicha_result_model(r),
			sicha_result_finish_reason_raw(r),
			(long long)sicha_result_total_tokens(r),
			(int)(len > 120 ? 120 : len), text);
	}
	sicha_result_destroy(r);
	sicha_client_destroy(client);
}

typedef struct live_deltas {
	char text[8192];
	size_t len;
	int calls;
	int cancel_after; /* 0 = never */
} live_deltas;

static int32_t live_on_delta(void *ud, const char *bytes, size_t len)
{
	live_deltas *d = ud;

	if (d->len + len < sizeof(d->text)) {
		memcpy(d->text + d->len, bytes, len);
		d->len += len;
	}
	d->calls++;
	return d->cancel_after != 0 && d->calls >= d->cancel_after ? 1 :
		0;
}

static void check_live_streaming(const live_cfg *cfg)
{
	sicha_client *client = live_client(cfg);
	sicha_message msg = { SICHA_ROLE_USER,
		"Count from one to five in words.", SICHA_LEN_CSTR,
		NULL };
	sicha_request req = live_request(&msg, 1, 64);
	live_deltas deltas;
	sicha_callbacks cbs;
	sicha_result *r = NULL;
	sicha_status st;

	memset(&deltas, 0, sizeof(deltas));
	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = &deltas;
	cbs.on_delta = live_on_delta;
	st = sicha_chat_stream(client, &req, &cbs, NULL, &r);

	T_CHECK(st == SICHA_OK);
	if (st != SICHA_OK) {
		report_attempts(r);
	} else {
		size_t len = 0;
		const char *text = sicha_result_text(r, &len);

		T_CHECK(deltas.calls >= 1);
		T_CHECK(deltas.len > 0);
		/* accumulated result text == what the callback saw */
		T_CHECK(len == deltas.len &&
			memcmp(text, deltas.text, len) == 0);
		/* a finished stream carries a finish_reason */
		T_CHECK(sicha_result_finish_reason_raw(r)[0] != '\0');
		fprintf(stderr, "live streaming: %d delta callbacks, "
			"finish=%s, text=%.*s\n", deltas.calls,
			sicha_result_finish_reason_raw(r),
			(int)(len > 120 ? 120 : len), text);
	}
	sicha_result_destroy(r);
	sicha_client_destroy(client);
}

static void check_live_cancellation(const live_cfg *cfg)
{
	sicha_client *client = live_client(cfg);
	sicha_message msg = { SICHA_ROLE_USER,
		"Write a very long story about the sea.",
		SICHA_LEN_CSTR, NULL };
	sicha_request req = live_request(&msg, 1, 512);
	live_deltas deltas;
	sicha_callbacks cbs;
	sicha_result *r = NULL;
	uint64_t t0;
	sicha_status st;

	memset(&deltas, 0, sizeof(deltas));
	deltas.cancel_after = 3; /* bail as soon as tokens flow */
	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = &deltas;
	cbs.on_delta = live_on_delta;
	t0 = 0;
	st = sicha_chat_stream(client, &req, &cbs, NULL, &r);
	(void)t0;

	T_CHECK(st == SICHA_E_CANCELLED);
	if (st != SICHA_E_CANCELLED) {
		report_attempts(r);
	} else {
		const sicha_attempt *a = sicha_result_attempt(r, 0);

		T_CHECK(deltas.calls >= 3);
		T_CHECK(a != NULL &&
			a->error_class == SICHA_CLASS_CANCELLED);
		fprintf(stderr, "live cancellation: stopped after %d "
			"deltas (%zu bytes)%s\n", deltas.calls,
			deltas.len, cfg->kobold_assist ?
				", abort assist fired" : "");
	}
	sicha_result_destroy(r);
	sicha_client_destroy(client);
}

int main(void)
{
	live_cfg cfg;

	cfg.base_url = getenv("SICHA_LIVE_BASE_URL");
	cfg.model = getenv("SICHA_LIVE_MODEL");
	cfg.api_key = getenv("SICHA_LIVE_API_KEY");
	if (cfg.api_key != NULL && cfg.api_key[0] == '\0') {
		cfg.api_key = NULL;
	}
	{
		const char *assist = getenv("SICHA_LIVE_KOBOLD_ASSIST");

		cfg.kobold_assist = assist != NULL &&
			strcmp(assist, "1") == 0;
	}
	if (cfg.base_url == NULL || cfg.base_url[0] == '\0' ||
		cfg.model == NULL || cfg.model[0] == '\0') {
		fprintf(stderr, "live: SICHA_LIVE_BASE_URL and "
			"SICHA_LIVE_MODEL not set — skipping\n");
		return LIVE_SKIP;
	}
	fprintf(stderr, "live: %s model=%s auth=%s assist=%d\n",
		cfg.base_url, cfg.model,
		cfg.api_key != NULL ? "bearer" : "none",
		cfg.kobold_assist);

	check_live_blocking(&cfg);
	check_live_streaming(&cfg);
	check_live_cancellation(&cfg);
	return t_done("test_live");
}
