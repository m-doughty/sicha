/* Engine backoff behavior under a fake clock: exact jittered wait
 * sequences (replicating the documented per-request splitmix64
 * stream), Retry-After substitution and capping, jitter disable, and
 * zero-wait validation re-rolls. */

#include <stdlib.h>

#include "engine_helpers.h"

static void check_exact_backoff_sequence(void)
{
	sicha_retry_policy pol;
	t_env e;
	sicha_result *r = NULL;

	memset(&pol, 0, sizeof(pol));
	pol.max_tries = 4;
	T_CHECK(t_env_init(&e, 1, &pol));
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_ok(e.script, T_OK_BODY("recovered"));

	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(strcmp(sicha_result_text(r, NULL), "recovered") == 0);
	T_CHECK(sicha_result_attempt_count(r) == 4);

	/* replicate the jitter stream: seed 42, request ordinal 0 */
	T_CHECK(e.fc.wait_calls == 3);
	{
		uint64_t expect[3];

		for (int k = 1; k <= 3; k++) {
			uint64_t base = 1000ull << (k - 1);
			uint64_t j = t_jitter_stream(42, 0, k - 1) % 500;
			uint64_t w = base + j;

			expect[k - 1] = w < 30000 ? w : 30000;
		}
		T_CHECK(e.fc.waits[0] == expect[0]);
		T_CHECK(e.fc.waits[1] == expect[1]);
		T_CHECK(e.fc.waits[2] == expect[2]);
		T_CHECK(e.fc.total_waited ==
			expect[0] + expect[1] + expect[2]);
	}
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_jitter_disabled_pure_powers(void)
{
	sicha_retry_policy pol;
	t_env e;
	sicha_result *r = NULL;

	memset(&pol, 0, sizeof(pol));
	pol.max_tries = 4;
	pol.backoff_jitter_ms = SICHA_DISABLED;
	T_CHECK(t_env_init(&e, 1, &pol));
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_ok(e.script, T_OK_BODY("x"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(e.fc.wait_calls == 3);
	T_CHECK(e.fc.waits[0] == 1000);
	T_CHECK(e.fc.waits[1] == 2000);
	T_CHECK(e.fc.waits[2] == 4000);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_retry_after_substitution(void)
{
	sicha_retry_policy pol;
	t_env e;
	sicha_result *r = NULL;
	sicha_header ra = { "Retry-After", "7" };

	memset(&pol, 0, sizeof(pol));
	pol.backoff_jitter_ms = SICHA_DISABLED;
	T_CHECK(t_env_init(&e, 1, &pol));
	/* 503 is RETRY_SAME by default: Retry-After replaces the
	 * formula exactly */
	t_push_http(e.script, 503, "overloaded", &ra, 1);
	t_push_ok(e.script, T_OK_BODY("x"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(e.fc.wait_calls == 1);
	T_CHECK(e.fc.waits[0] == 7000);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_retry_after_capped(void)
{
	sicha_retry_policy pol;
	t_env e;
	sicha_result *r = NULL;
	sicha_header ra = { "Retry-After", "9999" };

	memset(&pol, 0, sizeof(pol));
	pol.retry_after_cap_ms = 5000;
	T_CHECK(t_env_init(&e, 1, &pol));
	t_push_http(e.script, 503, "overloaded", &ra, 1);
	t_push_ok(e.script, T_OK_BODY("x"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(e.fc.waits[0] == 5000);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_retry_after_ignored_when_disabled(void)
{
	sicha_retry_policy pol;
	t_env e;
	sicha_result *r = NULL;
	sicha_header ra = { "Retry-After", "9999" };

	memset(&pol, 0, sizeof(pol));
	pol.retry_after_cap_ms = SICHA_DISABLED;
	pol.backoff_jitter_ms = SICHA_DISABLED;
	T_CHECK(t_env_init(&e, 1, &pol));
	t_push_http(e.script, 503, "overloaded", &ra, 1);
	t_push_ok(e.script, T_OK_BODY("x"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	/* the formula, not the header */
	T_CHECK(e.fc.waits[0] == 1000);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_garbage_retry_after_falls_back(void)
{
	sicha_retry_policy pol;
	t_env e;
	sicha_result *r = NULL;
	sicha_header ra = { "Retry-After",
		"Wed, 21 Oct 2015 07:28:00 GMT" };

	memset(&pol, 0, sizeof(pol));
	pol.backoff_jitter_ms = SICHA_DISABLED;
	T_CHECK(t_env_init(&e, 1, &pol));
	t_push_http(e.script, 500, "boom", &ra, 1);
	t_push_ok(e.script, T_OK_BODY("x"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(e.fc.waits[0] == 1000);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_429_override_honors_retry_after(void)
{
	sicha_retry_policy pol;
	sicha_status_override ov = { 429, SICHA_CLASS_RETRY_SAME };
	t_env e;
	sicha_result *r = NULL;
	sicha_header ra = { "Retry-After", "3" };

	memset(&pol, 0, sizeof(pol));
	pol.overrides = &ov;
	pol.n_overrides = 1;
	T_CHECK(t_env_init(&e, 1, &pol));
	t_push_http(e.script, 429, "slow down", &ra, 1);
	t_push_ok(e.script, T_OK_BODY("x"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(sicha_result_attempt_count(r) == 2);
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_RETRY_SAME);
	T_CHECK(e.fc.wait_calls == 1);
	T_CHECK(e.fc.waits[0] == 3000);
	sicha_result_destroy(r);
	t_env_free(&e);
}

static int32_t reject_first_n(void *ud, const sicha_result *r)
{
	int *n = ud;

	(void)r;
	if (*n > 0) {
		(*n)--;
		return 1;
	}
	return 0;
}

static void check_validation_rerolls_have_no_backoff(void)
{
	sicha_retry_policy pol;
	t_env e;
	sicha_result *r = NULL;
	sicha_callbacks cbs;
	int rejections = 2;

	memset(&pol, 0, sizeof(pol));
	pol.validation_retries = 2;
	T_CHECK(t_env_init(&e, 1, &pol));
	t_push_ok(e.script, T_OK_BODY("first"));
	t_push_ok(e.script, T_OK_BODY("second"));
	t_push_ok(e.script, T_OK_BODY("third"));

	memset(&cbs, 0, sizeof(cbs));
	cbs.struct_size = (uint32_t)sizeof(cbs);
	cbs.ud = &rejections;
	cbs.validate = reject_first_n;
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, &cbs, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(strcmp(sicha_result_text(r, NULL), "third") == 0);
	T_CHECK(sicha_result_attempt_count(r) == 3);
	T_CHECK(sicha_result_attempt(r, 0)->error_class ==
		SICHA_CLASS_VALIDATION);
	T_CHECK(sicha_result_attempt(r, 1)->error_class ==
		SICHA_CLASS_VALIDATION);
	T_CHECK(sicha_result_attempt(r, 2)->error_class ==
		SICHA_CLASS_NONE);
	/* the whole point: zero waits */
	T_CHECK(e.fc.wait_calls == 0);
	sicha_result_destroy(r);
	t_env_free(&e);
}

int main(void)
{
	check_exact_backoff_sequence();
	check_jitter_disabled_pure_powers();
	check_retry_after_substitution();
	check_retry_after_capped();
	check_retry_after_ignored_when_disabled();
	check_garbage_retry_after_falls_back();
	check_429_override_honors_retry_after();
	check_validation_rerolls_have_no_backoff();
	return t_done("test_backoff");
}
