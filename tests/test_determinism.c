/* Retry schedules are exactly reproducible: same seed => identical
 * wait sequences across runs; per-request jitter streams are
 * independent; a chosen different seed diverges (fixed pair, so the
 * check is deterministic). */

#include <stdlib.h>

#include "engine_helpers.h"

/* Run a fixed 3-failure scenario and capture the wait schedule. */
static void run_schedule(uint64_t seed, uint64_t waits_out[3],
	int *n_waits)
{
	t_env e;
	sicha_retry_policy pol;
	sicha_result *r = NULL;

	memset(&pol, 0, sizeof(pol));
	pol.max_tries = 4;
	pol.backoff_jitter_ms = 20000; /* wide: divergence is visible */
	pol.backoff_cap_ms = 60000;
	T_CHECK(t_env_init_ex(&e, 1, &pol, seed, 0));
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_ok(e.script, T_OK_BODY("done"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	*n_waits = e.fc.wait_calls;
	for (int i = 0; i < 3 && i < e.fc.wait_calls; i++) {
		waits_out[i] = e.fc.waits[i];
	}
	sicha_result_destroy(r);
	t_env_free(&e);
}

static void check_same_seed_identical(void)
{
	uint64_t a[3] = { 0, 0, 0 };
	uint64_t b[3] = { 0, 0, 0 };
	int na = 0;
	int nb = 0;

	run_schedule(1234, a, &na);
	run_schedule(1234, b, &nb);
	T_CHECK(na == 3 && nb == 3);
	T_CHECK(a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
}

static void check_chosen_seeds_diverge(void)
{
	uint64_t a[3] = { 0, 0, 0 };
	uint64_t b[3] = { 0, 0, 0 };
	int na = 0;
	int nb = 0;

	run_schedule(1234, a, &na);
	run_schedule(5678, b, &nb);
	T_CHECK(na == 3 && nb == 3);
	/* fixed seed pair chosen so the schedules differ — this is a
	 * deterministic assertion, not a probabilistic one */
	T_CHECK(a[0] != b[0] || a[1] != b[1] || a[2] != b[2]);
}

static void check_per_request_streams_differ(void)
{
	/* two requests on ONE client draw from different jitter
	 * streams (seed ^ request ordinal) */
	t_env e;
	sicha_retry_policy pol;
	uint64_t first_wait[2] = { 0, 0 };

	memset(&pol, 0, sizeof(pol));
	pol.backoff_jitter_ms = 20000;
	pol.backoff_cap_ms = 60000;
	T_CHECK(t_env_init_ex(&e, 1, &pol, 1234, 0));
	for (int q = 0; q < 2; q++) {
		sicha_result *r = NULL;
		sicha_request req = t_req1();
		int before = e.fc.wait_calls;

		t_push_terr(e.script, SICHA_T_E_RESET);
		t_push_ok(e.script, T_OK_BODY("x"));
		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
		T_CHECK(e.fc.wait_calls == before + 1);
		first_wait[q] = e.fc.waits[before];
		sicha_result_destroy(r);
	}
	T_CHECK(first_wait[0] != first_wait[1]);
	/* and both match the documented derivation exactly */
	T_CHECK(first_wait[0] ==
		1000 + t_jitter_stream(1234, 0, 0) % 20000);
	T_CHECK(first_wait[1] ==
		1000 + t_jitter_stream(1234, 1, 0) % 20000);
	t_env_free(&e);
}

static void check_entropy_seed_still_works(void)
{
	/* seed 0 = entropy: not asserting values, just that the engine
	 * runs and produces a legal schedule */
	t_env e;
	sicha_retry_policy pol;
	sicha_result *r = NULL;

	memset(&pol, 0, sizeof(pol));
	pol.backoff_jitter_ms = SICHA_DISABLED;
	T_CHECK(t_env_init_ex(&e, 1, &pol, 0, 0));
	t_push_terr(e.script, SICHA_T_E_RESET);
	t_push_ok(e.script, T_OK_BODY("x"));
	{
		sicha_request req = t_req1();

		T_CHECK(sicha_chat(e.client, &req, NULL, NULL, &r) ==
			SICHA_OK);
	}
	T_CHECK(e.fc.waits[0] == 1000); /* jitter off: deterministic */
	sicha_result_destroy(r);
	t_env_free(&e);
}

int main(void)
{
	check_same_seed_identical();
	check_chosen_seeds_diverge();
	check_per_request_streams_differ();
	check_entropy_seed_still_works();
	return t_done("test_determinism");
}
