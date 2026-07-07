/* libFuzzer harness for the retry/fallback engine (white-box).  Fuzz
 * bytes script an entire scenario: 1-3 backends, policy knobs
 * (budgets, jitter, flags, one override), and a queue of scripted
 * transport responses (statuses, HTTP codes, bodies, chunk sizes,
 * virtual delays, mid-body failures) — then one chat or chat_stream
 * call runs against a fake clock, with callbacks that sometimes
 * cancel and a validator that sometimes rejects.
 *
 * Oracles:
 *   - terminal status is in the legal set and matches result_status;
 *   - total attempts <= n_backends * (max_tries + validation_retries
 *     + 1) and backend indices are non-decreasing;
 *   - try_of_backend restarts at 0 per backend and increments;
 *   - every getter is safe on the final result; destroy is clean
 *     (leaks surface via ASan).
 *
 * Build with -DSICHA_FUZZ=ON (Clang only); run e.g.
 *   ./fuzz_client -max_total_time=60 corpus/
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

typedef struct rd {
	const uint8_t *d;
	size_t n;
	size_t i;
} rd;

static uint8_t r8(rd *r)
{
	return r->i < r->n ? r->d[r->i++] : 0;
}

static uint16_t r16(rd *r)
{
	return (uint16_t)((uint16_t)r8(r) | ((uint16_t)r8(r) << 8));
}

/* --- fuzz-controlled fake clock (no Date/rand: pure virtual time) -- */

typedef struct fz_clock {
	sicha_clock clock;
	uint64_t now;
	sicha_cancel *trigger;
	int trigger_at;
	int wait_calls;
} fz_clock;

static uint64_t fz_now(void *ud)
{
	return ((fz_clock *)ud)->now;
}

static int32_t fz_wait(void *ud, sicha_cancel *cancel, uint64_t ms)
{
	fz_clock *fc = ud;

	fc->wait_calls++;
	if (fc->trigger != NULL && fc->wait_calls >= fc->trigger_at) {
		sicha_cancel_trigger(fc->trigger);
	}
	if (cancel != NULL && sicha_cancel_is_cancelled(cancel)) {
		return 1;
	}
	fc->now += ms;
	return 0;
}

/* --- fuzz-controlled callbacks ------------------------------------- */

typedef struct fz_cbs_state {
	int cancel_delta_after;     /* 0 = never                        */
	int deltas;
	int reject_validations;     /* countdown                        */
	int attempts_seen;
	uint32_t last_backend;
	uint32_t expect_try;
	int backend_regressed;
} fz_cbs_state;

static int32_t fz_on_delta(void *ud, const char *bytes, size_t len)
{
	fz_cbs_state *s = ud;

	(void)bytes;
	(void)len;
	s->deltas++;
	return s->cancel_delta_after != 0 &&
		s->deltas >= s->cancel_delta_after ? 1 : 0;
}

static int32_t fz_on_raw(void *ud, const char *json, size_t len)
{
	(void)ud;
	FUZZ_ASSERT(json != NULL || len == 0);
	return 0;
}

static void fz_on_attempt(void *ud, const sicha_attempt *a)
{
	fz_cbs_state *s = ud;

	FUZZ_ASSERT(a != NULL);
	FUZZ_ASSERT(a->struct_size == (uint32_t)sizeof(*a));
	FUZZ_ASSERT(a->model != NULL && a->message != NULL);
	FUZZ_ASSERT(a->attempt == (uint32_t)s->attempts_seen);
	if (s->attempts_seen > 0 && a->backend < s->last_backend) {
		s->backend_regressed = 1;
	}
	if (s->attempts_seen == 0 || a->backend != s->last_backend) {
		s->expect_try = 0;
	}
	FUZZ_ASSERT(a->try_of_backend == s->expect_try);
	s->expect_try++;
	s->last_backend = a->backend;
	s->attempts_seen++;
}

static int32_t fz_validate(void *ud, const sicha_result *r)
{
	fz_cbs_state *s = ud;

	FUZZ_ASSERT(sicha_result_text(r, NULL) != NULL);
	if (s->reject_validations > 0) {
		s->reject_validations--;
		return 1;
	}
	return 0;
}

/* --- scripted wire payloads ---------------------------------------- */

static const char *OK_JSON = "{\"model\":\"srv\",\"choices\":[{"
	"\"message\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}";
static const char *SSE_OK = "data: {\"choices\":[{\"delta\":"
	"{\"content\":\"o\"}}]}\n\ndata: {\"choices\":[{\"delta\":"
	"{\"content\":\"k\"},\"finish_reason\":\"stop\"}]}\n\n"
	"data: [DONE]\n\n";
static const char *SSE_PARTIAL = "data: {\"choices\":[{\"delta\":"
	"{\"content\":\"pa\"}}]}\n\n";
static const char *MALFORMED = "<html>gateway</html>";

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	rd r = { data, size, 0 };
	sicha_backend_desc descs[3];
	sicha_status_override ov;
	sicha_retry_policy pol;
	sicha_client_opts opts;
	sicha_client *client = NULL;
	sicha_transport *script;
	fz_clock fc;
	fz_cbs_state cst;
	sicha_callbacks cbs;
	sicha_cancel *cancel = NULL;
	size_t n_backends;
	int stream;
	uint32_t budget;

	if (size < 8) {
		return 0;
	}
	script = sicha_script_create();
	FUZZ_ASSERT(script != NULL);
	memset(&fc, 0, sizeof(fc));
	fc.clock.ud = &fc;
	fc.clock.now_ms = fz_now;
	fc.clock.wait_ms = fz_wait;

	n_backends = 1 + r8(&r) % 3;
	for (size_t i = 0; i < n_backends; i++) {
		static const char *urls[3] = { "http://a.local/v1",
			"http://b.local/v1", "http://c.local/v1" };
		static const char *models[3] = { "m0", "m1", "m2" };

		memset(&descs[i], 0, sizeof(descs[i]));
		descs[i].struct_size = (uint32_t)sizeof(descs[i]);
		descs[i].base_url = urls[i];
		descs[i].model = models[i];
		descs[i].flags = r8(&r) % 4 == 0 ?
			SICHA_BACKEND_KOBOLD_CANCEL_ASSIST : 0;
		if (r8(&r) % 4 == 0) {
			descs[i].timeouts.connect_ms = 1 + r16(&r);
			descs[i].timeouts.total_ms = 1 + r16(&r);
		}
	}
	memset(&pol, 0, sizeof(pol));
	pol.max_tries = r8(&r) % 5;               /* 0 = default 3     */
	pol.validation_retries = r8(&r) % 3;
	pol.backoff_base_ms = r8(&r) % 2 == 0 ? 1 : 0;
	pol.backoff_jitter_ms = r8(&r) % 3 == 0 ? SICHA_DISABLED :
		(uint32_t)(r8(&r) % 64);
	pol.flags = 0;
	if (r8(&r) % 2 == 0) {
		pol.flags |= SICHA_POLICY_RETRY_AFTER_DELTAS;
	}
	if (r8(&r) % 4 == 0) {
		pol.flags |= SICHA_POLICY_LENGTH_IS_ADVANCE;
	}
	if (r8(&r) % 8 == 0) {
		static const sicha_error_class classes[3] = {
			SICHA_CLASS_ABORT, SICHA_CLASS_RETRY_SAME,
			SICHA_CLASS_ADVANCE };

		ov.http_status = 400 + r8(&r) % 200;
		ov.cls = classes[r8(&r) % 3];
		pol.overrides = &ov;
		pol.n_overrides = 1;
	}

	memset(&opts, 0, sizeof(opts));
	opts.struct_size = (uint32_t)sizeof(opts);
	opts.backends = descs;
	opts.n_backends = n_backends;
	opts.retry = pol;
	opts.transport = script;
	opts.clock = &fc.clock;
	opts.prng_seed = 1 + r16(&r);
	opts.max_response_bytes = r8(&r) % 8 == 0 ? 64 : 0;
	if (sicha_client_create(&opts, &client) != SICHA_OK) {
		sicha_script_destroy(script);
		return 0;
	}

	stream = r8(&r) % 2;
	{
		size_t n_resp = r8(&r) % 9;

		for (size_t q = 0; q < n_resp; q++) {
			sicha_script_response resp;
			uint8_t kind = r8(&r);

			memset(&resp, 0, sizeof(resp));
			resp.struct_size = (uint32_t)sizeof(resp);
			switch (kind % 8) {
			case 0: /* good response */
				resp.status = SICHA_T_OK;
				resp.http_status = 200;
				resp.body = stream ? SSE_OK : OK_JSON;
				resp.body_len = SICHA_LEN_CSTR;
				resp.chunk_size = 1 + r8(&r) % 32;
				break;
			case 1: /* http error, maybe with Retry-After */
				resp.status = SICHA_T_OK;
				resp.http_status = 100 + r8(&r) % 500;
				resp.body = "{\"error\":\"e\"}";
				resp.body_len = SICHA_LEN_CSTR;
				break;
			case 2: /* pure transport error */
				resp.status = -(int32_t)(1 + r8(&r) % 12);
				break;
			case 3: /* delays vs timeout budgets */
				resp.status = SICHA_T_OK;
				resp.http_status = 200;
				resp.body = stream ? SSE_OK : OK_JSON;
				resp.body_len = SICHA_LEN_CSTR;
				resp.connect_delay_ms = r16(&r);
				resp.first_byte_delay_ms = r16(&r);
				resp.per_chunk_delay_ms = r8(&r);
				resp.chunk_size = 1 + r8(&r) % 16;
				break;
			case 4: /* partial stream then death */
				resp.status = -(int32_t)(1 + r8(&r) % 12);
				resp.http_status = 200;
				resp.body = stream ? SSE_PARTIAL : OK_JSON;
				resp.body_len = SICHA_LEN_CSTR;
				resp.fail_after_bytes = 1 +
					r8(&r) % 64;
				break;
			case 5: /* malformed 200 */
				resp.status = SICHA_T_OK;
				resp.http_status = 200;
				resp.body = MALFORMED;
				resp.body_len = SICHA_LEN_CSTR;
				break;
			case 6: /* empty 200 */
				resp.status = SICHA_T_OK;
				resp.http_status = 200;
				break;
			default: /* truncation via finish_reason */
				resp.status = SICHA_T_OK;
				resp.http_status = 200;
				resp.body = "{\"choices\":[{\"message\":"
					"{\"content\":\"t\"},"
					"\"finish_reason\":\"length\"}]}";
				resp.body_len = SICHA_LEN_CSTR;
				break;
			}
			if (sicha_script_push(script, &resp) != SICHA_OK) {
				break;
			}
		}
	}

	memset(&cst, 0, sizeof(cst));
	cst.cancel_delta_after = r8(&r) % 8 == 0 ? 1 + r8(&r) % 3 : 0;
	cst.reject_validations = r8(&r) % 4 == 0 ? r8(&r) % 3 : 0;
	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = &cst;
	cbs.on_delta = r8(&r) % 2 == 0 ? fz_on_delta : NULL;
	cbs.on_raw_chunk = fz_on_raw;
	cbs.on_attempt = fz_on_attempt;
	cbs.validate = r8(&r) % 2 == 0 ? fz_validate : NULL;

	if (r8(&r) % 4 == 0) {
		cancel = sicha_cancel_create();
		FUZZ_ASSERT(cancel != NULL);
		fc.trigger = cancel;
		fc.trigger_at = 1 + r8(&r) % 8;
	}

	{
		sicha_message msg = { SICHA_ROLE_USER, "q",
			SICHA_LEN_CSTR, NULL };
		sicha_request req;
		sicha_result *res = NULL;
		sicha_status st;
		uint32_t resolved_tries = pol.max_tries == 0 ?
			SICHA_DEFAULT_MAX_TRIES : pol.max_tries;

		memset(&req, 0, sizeof(req));
		req.struct_size = (uint32_t)sizeof(req);
		req.messages = &msg;
		req.n_messages = 1;
		req.flags = r8(&r) % 8 == 0 ? SICHA_REQ_NO_ACCUMULATE : 0;

		st = stream ?
			sicha_chat_stream(client, &req,
				r8(&r) % 8 == 0 ? NULL : &cbs, cancel,
				&res) :
			sicha_chat(client, &req,
				r8(&r) % 8 == 0 ? NULL : &cbs, cancel,
				&res);

		FUZZ_ASSERT(st == SICHA_OK || st == SICHA_E_ABORTED ||
			st == SICHA_E_EXHAUSTED ||
			st == SICHA_E_CANCELLED ||
			st == SICHA_E_STREAM_LOST ||
			st == SICHA_E_NOMEM);
		FUZZ_ASSERT(res != NULL);
		FUZZ_ASSERT(sicha_result_status(res) == st);
		budget = (uint32_t)n_backends *
			(resolved_tries + pol.validation_retries + 1);
		FUZZ_ASSERT(sicha_result_attempt_count(res) <= budget);
		FUZZ_ASSERT(!cst.backend_regressed);
		{
			size_t len = 0;
			const char *s = sicha_result_text(res, &len);

			FUZZ_ASSERT(s != NULL && s[len] == '\0');
			s = sicha_result_reasoning(res, &len);
			FUZZ_ASSERT(s != NULL && s[len] == '\0');
			FUZZ_ASSERT(sicha_result_model(res) != NULL);
			FUZZ_ASSERT(
				sicha_result_finish_reason_raw(res) !=
				NULL);
		}
		for (uint32_t i = 0;
			i < sicha_result_attempt_count(res); i++) {
			const sicha_attempt *a =
				sicha_result_attempt(res, i);

			FUZZ_ASSERT(a != NULL);
			FUZZ_ASSERT(a->attempt == i);
			FUZZ_ASSERT(a->backend < n_backends);
			FUZZ_ASSERT(a->raw_body_excerpt_len <=
				SICHA_EXCERPT_MAX);
		}
		sicha_result_destroy(res);
	}

	sicha_cancel_destroy(cancel);
	sicha_client_destroy(client);
	sicha_script_destroy(script);
	return 0;
}
