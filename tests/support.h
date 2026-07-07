/* Shared test support: tiny check harness and a seeded PRNG.
 * Grows builder helpers (clients, backends, fake clocks) as the
 * suites need them. */

#ifndef SICHA_TEST_SUPPORT_H
#define SICHA_TEST_SUPPORT_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sicha.h"

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */

extern int t_checks;
extern int t_fails;

void t_check_impl(int ok, const char *expr, const char *file, int line);
int t_done(const char *suite); /* prints summary, returns exit code */

#define T_CHECK(cond) t_check_impl((cond) ? 1 : 0, #cond, __FILE__, __LINE__)

/* ------------------------------------------------------------------ */
/* Deterministic PRNG (splitmix64) — never seeded from the clock       */
/* ------------------------------------------------------------------ */

typedef struct t_rng {
	uint64_t s;
} t_rng;

uint64_t t_rng_next(t_rng *r);
uint32_t t_rng_below(t_rng *r, uint32_t bound);

/* ------------------------------------------------------------------ */
/* Fake clock — virtual time; waits are instant in real time           */
/* ------------------------------------------------------------------ */

#define T_FAKE_CLOCK_MAX_WAITS 64

/* NOT thread-safe: single-threaded suites only (the threaded suite
 * runs on the real clock). */
typedef struct t_fake_clock {
	sicha_clock clock;      /* vtable; ud points back at this       */
	uint64_t now;           /* virtual milliseconds                 */
	uint64_t total_waited;
	int wait_calls;
	uint64_t waits[T_FAKE_CLOCK_MAX_WAITS]; /* individual durations */
	/* when set, trigger this token once wait_calls reaches the
	 * threshold (simulates cancellation racing a sleep) */
	sicha_cancel *trigger_on_wait;
	int trigger_at_call;
} t_fake_clock;

/* Initialize and return the embedded vtable pointer. */
const sicha_clock *t_fake_clock_init(t_fake_clock *fc, uint64_t start);

#endif /* SICHA_TEST_SUPPORT_H */
