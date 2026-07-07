/* Concurrency: many threads sharing ONE client (concurrent performs
 * through the scripted transport), cross-thread cancellation of an
 * in-flight stream, and one mass-cancel token shared by a fleet of
 * requests.  Runs on the REAL clock (the fake clock is documented
 * single-threaded); this suite is the primary tsan workload. */

#include <stdlib.h>

#include "engine_helpers.h"
#include "sicha_thread.h"

#define N_THREADS 8
#define REQS_PER_THREAD 4

typedef struct worker_arg {
	sicha_client *client;
	sicha_cancel *cancel;       /* NULL for the plain worker        */
	int ok;
	int cancelled;
	int other;
} worker_arg;

static void worker_plain(void *p)
{
	worker_arg *w = p;

	for (int i = 0; i < REQS_PER_THREAD; i++) {
		sicha_request req = t_req1();
		sicha_result *r = NULL;
		sicha_status st = sicha_chat(w->client, &req, NULL, NULL,
			&r);

		if (st == SICHA_OK &&
			strcmp(sicha_result_text(r, NULL), "shared") == 0 &&
			sicha_result_attempt_count(r) == 1) {
			w->ok++;
		} else {
			w->other++;
		}
		sicha_result_destroy(r);
	}
}

static void check_concurrent_performs(void)
{
	sicha_transport *script = sicha_script_create();
	sicha_backend_desc d = t_backend("http://b0.local/v1", "m0");
	sicha_client_opts opts;
	sicha_client *client = NULL;
	sicha_thread threads[N_THREADS];
	worker_arg args[N_THREADS];

	memset(&opts, 0, sizeof(opts));
	opts.struct_size = (uint32_t)sizeof(opts);
	opts.backends = &d;
	opts.n_backends = 1;
	opts.transport = script;
	opts.prng_seed = 42;
	/* real clock: opts.clock stays NULL */
	T_CHECK(sicha_client_create(&opts, &client) == SICHA_OK);

	for (int i = 0; i < N_THREADS * REQS_PER_THREAD; i++) {
		t_push_ok(script, T_OK_BODY("shared"));
	}
	for (int i = 0; i < N_THREADS; i++) {
		memset(&args[i], 0, sizeof(args[i]));
		args[i].client = client;
		T_CHECK(sicha_thread_create(&threads[i], worker_plain,
			&args[i]) == 1);
	}
	{
		int total_ok = 0;

		for (int i = 0; i < N_THREADS; i++) {
			sicha_thread_join(threads[i]);
			total_ok += args[i].ok;
			T_CHECK(args[i].other == 0);
		}
		T_CHECK(total_ok == N_THREADS * REQS_PER_THREAD);
	}
	T_CHECK(sicha_script_call_count(script) ==
		N_THREADS * REQS_PER_THREAD);
	sicha_client_destroy(client);
	sicha_script_destroy(script);
}

static void worker_cancellable(void *p)
{
	worker_arg *w = p;
	sicha_request req = t_req1();
	sicha_result *r = NULL;
	sicha_status st = sicha_chat_stream(w->client, &req, NULL,
		w->cancel, &r);

	if (st == SICHA_E_CANCELLED) {
		w->cancelled++;
	} else if (st == SICHA_OK) {
		w->ok++;
	} else {
		w->other++;
	}
	sicha_result_destroy(r);
}

static void check_mass_cancel(void)
{
	/* a fleet of streams stuck in a long connect delay; one token
	 * shared by all; a single trigger fells the lot */
	sicha_transport *script = sicha_script_create();
	sicha_backend_desc d = t_backend("http://b0.local/v1", "m0");
	sicha_client_opts opts;
	sicha_client *client = NULL;
	sicha_cancel *cancel = sicha_cancel_create();
	sicha_thread threads[N_THREADS];
	worker_arg args[N_THREADS];
	uint64_t t0;

	memset(&opts, 0, sizeof(opts));
	opts.struct_size = (uint32_t)sizeof(opts);
	opts.backends = &d;
	opts.n_backends = 1;
	opts.transport = script;
	opts.prng_seed = 42;
	T_CHECK(sicha_client_create(&opts, &client) == SICHA_OK);

	for (int i = 0; i < N_THREADS; i++) {
		sicha_script_response resp;

		memset(&resp, 0, sizeof(resp));
		resp.struct_size = (uint32_t)sizeof(resp);
		resp.status = SICHA_T_OK;
		resp.http_status = 200;
		resp.body = T_SSE_BODY("a", "b");
		resp.body_len = SICHA_LEN_CSTR;
		resp.connect_delay_ms = 30000; /* would take 30 s */
		T_CHECK(sicha_script_push(script, &resp) == SICHA_OK);
	}
	for (int i = 0; i < N_THREADS; i++) {
		memset(&args[i], 0, sizeof(args[i]));
		args[i].client = client;
		args[i].cancel = cancel;
		T_CHECK(sicha_thread_create(&threads[i],
			worker_cancellable, &args[i]) == 1);
	}
	/* let them get in flight, then cut them all down */
	sicha_clock_real.wait_ms(NULL, NULL, 150);
	t0 = sicha_clock_real.now_ms(NULL);
	sicha_cancel_trigger(cancel);
	{
		int total_cancelled = 0;

		for (int i = 0; i < N_THREADS; i++) {
			sicha_thread_join(threads[i]);
			total_cancelled += args[i].cancelled;
			T_CHECK(args[i].ok == 0 && args[i].other == 0);
		}
		T_CHECK(total_cancelled == N_THREADS);
	}
	/* everything unwound promptly (transport polls at ~100 ms);
	 * loose bound for noisy CI boxes */
	T_CHECK(sicha_clock_real.now_ms(NULL) - t0 < 10000);
	sicha_cancel_destroy(cancel);
	sicha_client_destroy(client);
	sicha_script_destroy(script);
}

static void worker_retry_storm(void *p)
{
	worker_arg *w = p;
	sicha_request req = t_req1();
	sicha_result *r = NULL;
	sicha_status st = sicha_chat(w->client, &req, NULL, NULL, &r);

	if (st == SICHA_OK &&
		strcmp(sicha_result_text(r, NULL), "shared") == 0) {
		w->ok++;
	} else {
		w->other++;
	}
	sicha_result_destroy(r);
}

static void check_concurrent_retry_storm(void)
{
	/* every request fails once then succeeds: concurrent backoffs
	 * (real clock, tiny base) exercise the wait path under tsan */
	sicha_transport *script = sicha_script_create();
	sicha_backend_desc d = t_backend("http://b0.local/v1", "m0");
	sicha_retry_policy pol;
	sicha_client_opts opts;
	sicha_client *client = NULL;
	sicha_thread threads[N_THREADS];
	worker_arg args[N_THREADS];

	memset(&pol, 0, sizeof(pol));
	pol.backoff_base_ms = 10;
	pol.backoff_cap_ms = 50;
	pol.backoff_jitter_ms = 10;
	memset(&opts, 0, sizeof(opts));
	opts.struct_size = (uint32_t)sizeof(opts);
	opts.backends = &d;
	opts.n_backends = 1;
	opts.retry = pol;
	opts.transport = script;
	opts.prng_seed = 42;
	T_CHECK(sicha_client_create(&opts, &client) == SICHA_OK);

	/* interleaving is nondeterministic, so every possible pop
	 * pattern must survive: with only 2 RESETs queued, even one
	 * thread eating both stays within its max_tries=3 budget */
	t_push_terr(script, SICHA_T_E_RESET);
	t_push_terr(script, SICHA_T_E_RESET);
	for (int i = 0; i < N_THREADS + 2; i++) {
		t_push_ok(script, T_OK_BODY("shared"));
	}
	for (int i = 0; i < N_THREADS; i++) {
		memset(&args[i], 0, sizeof(args[i]));
		args[i].client = client;
		T_CHECK(sicha_thread_create(&threads[i],
			worker_retry_storm, &args[i]) == 1);
	}
	{
		int total_ok = 0;

		for (int i = 0; i < N_THREADS; i++) {
			sicha_thread_join(threads[i]);
			total_ok += args[i].ok;
			T_CHECK(args[i].other == 0);
		}
		T_CHECK(total_ok == N_THREADS);
	}
	sicha_client_destroy(client);
	sicha_script_destroy(script);
}

int main(void)
{
	check_concurrent_performs();
	check_mass_cancel();
	check_concurrent_retry_storm();
	return t_done("test_threads");
}
