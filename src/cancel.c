/* The cancel token (atomic flag + mutex/condvar so triggering wakes
 * sleepers instantly) and the built-in real clock.
 *
 * Wake-latency contract: a wait_ms parked on a token's condvar wakes
 * on the trigger broadcast immediately; token-less waits chunk-sleep
 * at 100 ms so even a poll-only consumer sees cancellation fast. */

#include <stdlib.h>

#include "sicha_internal.h"
#include "sicha_thread.h"

struct sicha_cancel {
	sicha_mutex mu;
	sicha_cond cv;
	sicha_atomic_u32 flag;
};

sicha_cancel *sicha_cancel_create(void)
{
	sicha_cancel *c = calloc(1, sizeof(*c));

	if (c == NULL) {
		return NULL;
	}
	sicha_mutex_init(&c->mu);
	sicha_cond_init(&c->cv);
	sicha_atomic_store_u32(&c->flag, 0);
	return c;
}

void sicha_cancel_destroy(sicha_cancel *c)
{
	if (c == NULL) {
		return;
	}
	sicha_mutex_destroy(&c->mu);
	sicha_cond_destroy(&c->cv);
	free(c);
}

void sicha_cancel_trigger(sicha_cancel *c)
{
	if (c == NULL) {
		return;
	}
	/* the store happens under the mutex so a waiter cannot check
	 * the flag, decide to sleep, and miss the broadcast */
	sicha_mutex_lock(&c->mu);
	sicha_atomic_store_u32(&c->flag, 1);
	sicha_cond_broadcast(&c->cv);
	sicha_mutex_unlock(&c->mu);
}

void sicha_cancel_reset(sicha_cancel *c)
{
	if (c == NULL) {
		return;
	}
	sicha_mutex_lock(&c->mu);
	sicha_atomic_store_u32(&c->flag, 0);
	sicha_mutex_unlock(&c->mu);
}

int32_t sicha_cancel_is_cancelled(const sicha_cancel *c)
{
	if (c == NULL) {
		return 0;
	}
	/* the atomic makes the lock-free fast path legal */
	return sicha_atomic_load_u32(
		&((sicha_cancel *)(uintptr_t)c)->flag) != 0;
}

/* ------------------------------------------------------------------ */
/* Real clock                                                          */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)

static uint64_t real_now_ms(void *ud)
{
	(void)ud;
	return (uint64_t)GetTickCount64();
}

static void real_sleep_ms(uint64_t ms)
{
	Sleep(ms > 0xFFFFFFFEu ? 0xFFFFFFFEu : (DWORD)ms);
}

#else

static uint64_t real_now_ms(void *ud)
{
	struct timespec ts;

	(void)ud;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u +
		(uint64_t)(ts.tv_nsec / 1000000L);
}

static void real_sleep_ms(uint64_t ms)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(ms / 1000);
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

#endif

static int32_t real_wait_ms(void *ud, sicha_cancel *cancel, uint64_t ms)
{
	uint64_t deadline;
	uint64_t now;

	(void)ud;
	now = real_now_ms(NULL);
	deadline = ms >= SICHA_WAIT_FOREVER_MS ||
		now > UINT64_MAX - ms ? UINT64_MAX : now + ms;
	if (cancel == NULL) {
		/* nothing can wake us: bounded chunk sleeps */
		while (now < deadline) {
			uint64_t chunk = deadline - now;

			real_sleep_ms(chunk > 100 ? 100 : chunk);
			if (deadline == UINT64_MAX) {
				/* "forever" without a token would hang
				 * unrecoverably; treat as full chunks
				 * until the caller re-decides */
				return 0;
			}
			now = real_now_ms(NULL);
		}
		return 0;
	}
	sicha_mutex_lock(&cancel->mu);
	while (sicha_atomic_load_u32(&cancel->flag) == 0) {
		uint64_t remain;

		now = real_now_ms(NULL);
		if (now >= deadline) {
			break;
		}
		remain = deadline == UINT64_MAX ? SICHA_WAIT_FOREVER_MS :
			deadline - now;
		sicha_cond_timedwait_ms(&cancel->cv, &cancel->mu, remain);
	}
	{
		int32_t cancelled =
			sicha_atomic_load_u32(&cancel->flag) != 0;

		sicha_mutex_unlock(&cancel->mu);
		return cancelled;
	}
}

const sicha_clock sicha_clock_real = { NULL, real_now_ms, real_wait_ms };
