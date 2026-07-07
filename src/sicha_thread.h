/* Portable threading shim: threads, mutex, condvar, timed condvar
 * wait.  POSIX (pthreads) and Win32 (SRWLOCK + CONDITION_VARIABLE).
 * Everything is static inline, so no link artifacts.
 *
 * sicha_cond_timedwait_ms returns 1 when woken (signal, broadcast, or
 * spurious wake) and 0 on timeout.  Waits are bounded and re-checked
 * by callers, so wall-clock precision is not load-bearing; the POSIX
 * path uses CLOCK_REALTIME because macOS lacks
 * pthread_condattr_setclock. */

#ifndef SICHA_THREAD_H
#define SICHA_THREAD_H

#include <stdint.h>
#include <stdlib.h>

/* Waits at or above this are treated as "forever". */
#define SICHA_WAIT_FOREVER_MS ((uint64_t)1 << 60)

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <process.h>

typedef HANDLE sicha_thread;
typedef SRWLOCK sicha_mutex;
typedef CONDITION_VARIABLE sicha_cond;

static inline void sicha_mutex_init(sicha_mutex *m)
{
	InitializeSRWLock(m);
}

static inline void sicha_mutex_destroy(sicha_mutex *m)
{
	(void)m;
}

static inline void sicha_mutex_lock(sicha_mutex *m)
{
	AcquireSRWLockExclusive(m);
}

static inline void sicha_mutex_unlock(sicha_mutex *m)
{
	ReleaseSRWLockExclusive(m);
}

static inline void sicha_cond_init(sicha_cond *c)
{
	InitializeConditionVariable(c);
}

static inline void sicha_cond_destroy(sicha_cond *c)
{
	(void)c;
}

static inline void sicha_cond_wait(sicha_cond *c, sicha_mutex *m)
{
	SleepConditionVariableSRW(c, m, INFINITE, 0);
}

static inline int sicha_cond_timedwait_ms(sicha_cond *c, sicha_mutex *m,
	uint64_t ms)
{
	DWORD budget;

	if (ms >= SICHA_WAIT_FOREVER_MS) {
		SleepConditionVariableSRW(c, m, INFINITE, 0);
		return 1;
	}
	budget = ms > 0xFFFFFFFEu ? 0xFFFFFFFEu : (DWORD)ms;
	if (SleepConditionVariableSRW(c, m, budget, 0)) {
		return 1;
	}
	return GetLastError() == ERROR_TIMEOUT ? 0 : 1;
}

static inline void sicha_cond_signal(sicha_cond *c)
{
	WakeConditionVariable(c);
}

static inline void sicha_cond_broadcast(sicha_cond *c)
{
	WakeAllConditionVariable(c);
}

struct sicha_thread_boot {
	void (*fn)(void *);
	void *arg;
};

static inline unsigned __stdcall sicha_thread_tramp_(void *p)
{
	struct sicha_thread_boot b = *(struct sicha_thread_boot *)p;

	free(p);
	b.fn(b.arg);
	return 0;
}

static inline int sicha_thread_create(sicha_thread *t, void (*fn)(void *),
	void *arg)
{
	struct sicha_thread_boot *b = malloc(sizeof(*b));
	uintptr_t h;

	if (b == NULL) {
		return 0;
	}
	b->fn = fn;
	b->arg = arg;
	h = _beginthreadex(NULL, 0, sicha_thread_tramp_, b, 0, NULL);
	if (h == 0) {
		free(b);
		return 0;
	}
	*t = (HANDLE)h;
	return 1;
}

static inline void sicha_thread_join(sicha_thread t)
{
	WaitForSingleObject(t, INFINITE);
	CloseHandle(t);
}

#else /* POSIX */

#include <errno.h>
#include <pthread.h>
#include <time.h>

typedef pthread_t sicha_thread;
typedef pthread_mutex_t sicha_mutex;
typedef pthread_cond_t sicha_cond;

static inline void sicha_mutex_init(sicha_mutex *m)
{
	pthread_mutex_init(m, NULL);
}

static inline void sicha_mutex_destroy(sicha_mutex *m)
{
	pthread_mutex_destroy(m);
}

static inline void sicha_mutex_lock(sicha_mutex *m)
{
	pthread_mutex_lock(m);
}

static inline void sicha_mutex_unlock(sicha_mutex *m)
{
	pthread_mutex_unlock(m);
}

static inline void sicha_cond_init(sicha_cond *c)
{
	pthread_cond_init(c, NULL);
}

static inline void sicha_cond_destroy(sicha_cond *c)
{
	pthread_cond_destroy(c);
}

static inline void sicha_cond_wait(sicha_cond *c, sicha_mutex *m)
{
	pthread_cond_wait(c, m);
}

static inline int sicha_cond_timedwait_ms(sicha_cond *c, sicha_mutex *m,
	uint64_t ms)
{
	struct timespec ts;

	if (ms >= SICHA_WAIT_FOREVER_MS) {
		pthread_cond_wait(c, m);
		return 1;
	}
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += (time_t)(ms / 1000);
	ts.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}
	return pthread_cond_timedwait(c, m, &ts) == ETIMEDOUT ? 0 : 1;
}

static inline void sicha_cond_signal(sicha_cond *c)
{
	pthread_cond_signal(c);
}

static inline void sicha_cond_broadcast(sicha_cond *c)
{
	pthread_cond_broadcast(c);
}

struct sicha_thread_boot {
	void (*fn)(void *);
	void *arg;
};

static inline void *sicha_thread_tramp_(void *p)
{
	struct sicha_thread_boot b = *(struct sicha_thread_boot *)p;

	free(p);
	b.fn(b.arg);
	return NULL;
}

static inline int sicha_thread_create(sicha_thread *t, void (*fn)(void *),
	void *arg)
{
	struct sicha_thread_boot *b = malloc(sizeof(*b));

	if (b == NULL) {
		return 0;
	}
	b->fn = fn;
	b->arg = arg;
	if (pthread_create(t, NULL, sicha_thread_tramp_, b) != 0) {
		free(b);
		return 0;
	}
	return 1;
}

static inline void sicha_thread_join(sicha_thread t)
{
	pthread_join(t, NULL);
}

#endif

#endif /* SICHA_THREAD_H */
