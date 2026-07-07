#include "support.h"

int t_checks = 0;
int t_fails = 0;

void t_check_impl(int ok, const char *expr, const char *file, int line)
{
	t_checks++;
	if (!ok) {
		t_fails++;
		fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
	}
}

int t_done(const char *suite)
{
	fprintf(stderr, "%s: %d checks, %d failures\n", suite, t_checks,
		t_fails);
	return t_fails == 0 ? 0 : 1;
}

uint64_t t_rng_next(t_rng *r)
{
	uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);

	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

uint32_t t_rng_below(t_rng *r, uint32_t bound)
{
	return bound == 0 ? 0 : (uint32_t)(t_rng_next(r) % bound);
}

static uint64_t fake_now_ms(void *ud)
{
	t_fake_clock *fc = ud;

	return fc->now;
}

static int32_t fake_wait_ms(void *ud, sicha_cancel *cancel, uint64_t ms)
{
	t_fake_clock *fc = ud;

	fc->wait_calls++;
	if (fc->trigger_on_wait != NULL &&
		fc->wait_calls >= fc->trigger_at_call) {
		sicha_cancel_trigger(fc->trigger_on_wait);
	}
	if (cancel != NULL && sicha_cancel_is_cancelled(cancel)) {
		return 1;
	}
	if (fc->wait_calls <= T_FAKE_CLOCK_MAX_WAITS) {
		fc->waits[fc->wait_calls - 1] = ms;
	}
	fc->now += ms;
	fc->total_waited += ms;
	return 0;
}

const sicha_clock *t_fake_clock_init(t_fake_clock *fc, uint64_t start)
{
	memset(fc, 0, sizeof(*fc));
	fc->now = start;
	fc->clock.ud = fc;
	fc->clock.now_ms = fake_now_ms;
	fc->clock.wait_ms = fake_wait_ms;
	return &fc->clock;
}
