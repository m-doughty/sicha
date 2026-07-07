/* buf.c: growth, NUL-termination invariant, hard caps, overflow
 * rejection, sticky errors, take/reset semantics. */

#include <stdlib.h>

#include "sicha_internal.h"
#include "support.h"

static void check_basic(void)
{
	sicha_buf b;

	sicha_buf_init(&b, 0);
	T_CHECK(b.data == NULL);
	T_CHECK(b.len == 0);
	T_CHECK(sicha_buf_status(&b) == SICHA_OK);

	sicha_buf_append_cstr(&b, "hello");
	T_CHECK(b.len == 5);
	T_CHECK(b.data != NULL && strcmp(b.data, "hello") == 0);
	T_CHECK(b.data[b.len] == '\0');

	sicha_buf_append_ch(&b, ' ');
	sicha_buf_append(&b, "world", 5);
	T_CHECK(b.len == 11);
	T_CHECK(strcmp(b.data, "hello world") == 0);
	T_CHECK(sicha_buf_status(&b) == SICHA_OK);

	sicha_buf_reset(&b);
	T_CHECK(b.len == 0);
	T_CHECK(b.data != NULL && b.data[0] == '\0');
	T_CHECK(b.cap > 0); /* allocation kept */

	sicha_buf_free(&b);
	T_CHECK(b.data == NULL && b.len == 0 && b.cap == 0);
}

static void check_empty_append_materializes_nul(void)
{
	sicha_buf b;

	sicha_buf_init(&b, 0);
	sicha_buf_append(&b, "x", 0);
	T_CHECK(b.data != NULL);
	T_CHECK(b.data[0] == '\0');
	T_CHECK(b.len == 0);
	sicha_buf_free(&b);
}

static void check_growth(void)
{
	sicha_buf b;
	t_rng rng = { 42 };
	size_t total = 0;

	sicha_buf_init(&b, 0);
	for (int i = 0; i < 3000; i++) {
		char chunk[97];
		size_t n = 1 + t_rng_below(&rng, sizeof(chunk));

		for (size_t k = 0; k < n; k++) {
			chunk[k] = (char)('a' + (t_rng_next(&rng) % 26));
		}
		sicha_buf_append(&b, chunk, n);
		total += n;
		T_CHECK(b.len == total);
		T_CHECK(b.data[b.len] == '\0');
	}
	T_CHECK(sicha_buf_status(&b) == SICHA_OK);
	T_CHECK(b.cap >= b.len);
	sicha_buf_free(&b);
}

static void check_binary_content(void)
{
	sicha_buf b;
	static const char raw[] = { 'a', '\0', '\n', '\xFF', 'b' };

	sicha_buf_init(&b, 0);
	sicha_buf_append(&b, raw, sizeof(raw));
	T_CHECK(b.len == sizeof(raw));
	T_CHECK(memcmp(b.data, raw, sizeof(raw)) == 0);
	T_CHECK(b.data[b.len] == '\0');
	sicha_buf_free(&b);
}

static void check_max_len(void)
{
	sicha_buf b;

	sicha_buf_init(&b, 8);
	sicha_buf_append_cstr(&b, "12345678");
	T_CHECK(sicha_buf_status(&b) == SICHA_OK);
	T_CHECK(b.len == 8);

	sicha_buf_append_ch(&b, '9');
	T_CHECK(sicha_buf_status(&b) == SICHA_INT_E_TOOBIG);
	T_CHECK(b.len == 8); /* untouched */

	/* sticky: later appends are no-ops */
	sicha_buf_append_cstr(&b, "more");
	T_CHECK(b.len == 8);
	T_CHECK(sicha_buf_status(&b) == SICHA_INT_E_TOOBIG);

	/* reset clears content but NOT the sticky error */
	sicha_buf_reset(&b);
	T_CHECK(b.len == 0);
	T_CHECK(sicha_buf_status(&b) == SICHA_INT_E_TOOBIG);

	/* take on an errored buffer yields NULL and reinitializes */
	{
		size_t n = 77;
		char *p = sicha_buf_take(&b, &n);

		T_CHECK(p == NULL);
		T_CHECK(n == 0);
		T_CHECK(sicha_buf_status(&b) == SICHA_OK);
	}
	sicha_buf_free(&b);
}

static void check_max_len_exact_boundary(void)
{
	sicha_buf b;

	/* filling to exactly max_len is fine */
	sicha_buf_init(&b, 4);
	sicha_buf_append(&b, "ab", 2);
	sicha_buf_append(&b, "cd", 2);
	T_CHECK(sicha_buf_status(&b) == SICHA_OK);
	T_CHECK(b.len == 4);
	sicha_buf_free(&b);
}

static void check_overflow_rejected(void)
{
	sicha_buf b;

	sicha_buf_init(&b, 0);
	sicha_buf_append_cstr(&b, "seed");
	/* length arithmetic would overflow SIZE_MAX: must fail cleanly
	 * without touching the (bogus) source pointer */
	sicha_buf_append(&b, "", SIZE_MAX - 1);
	T_CHECK(sicha_buf_status(&b) == SICHA_E_NOMEM);
	T_CHECK(b.len == 4);
	sicha_buf_free(&b);

	/* SIZE_MAX itself */
	sicha_buf_init(&b, 0);
	sicha_buf_append(&b, "", SIZE_MAX);
	T_CHECK(sicha_buf_status(&b) == SICHA_E_NOMEM);
	sicha_buf_free(&b);
}

static void check_take(void)
{
	sicha_buf b;
	size_t n;
	char *p;

	sicha_buf_init(&b, 0);
	sicha_buf_append_cstr(&b, "payload");
	p = sicha_buf_take(&b, &n);
	T_CHECK(p != NULL);
	T_CHECK(n == 7);
	T_CHECK(strcmp(p, "payload") == 0);
	T_CHECK(b.data == NULL && b.len == 0);
	T_CHECK(sicha_buf_status(&b) == SICHA_OK);
	free(p);

	/* take on a never-touched buffer returns a malloc'd "" */
	p = sicha_buf_take(&b, &n);
	T_CHECK(p != NULL);
	T_CHECK(n == 0);
	T_CHECK(p[0] == '\0');
	free(p);

	/* len out-param is optional */
	sicha_buf_append_cstr(&b, "x");
	p = sicha_buf_take(&b, NULL);
	T_CHECK(p != NULL && strcmp(p, "x") == 0);
	free(p);

	/* max_len survives take's reinitialization */
	sicha_buf_free(&b);
	sicha_buf_init(&b, 2);
	sicha_buf_append_cstr(&b, "ab");
	p = sicha_buf_take(&b, NULL);
	T_CHECK(p != NULL);
	free(p);
	sicha_buf_append_cstr(&b, "cde");
	T_CHECK(sicha_buf_status(&b) == SICHA_INT_E_TOOBIG);
	sicha_buf_free(&b);
}

int main(void)
{
	check_basic();
	check_empty_append_materializes_nul();
	check_growth();
	check_binary_content();
	check_max_len();
	check_max_len_exact_boundary();
	check_overflow_rejected();
	check_take();
	return t_done("test_buf");
}
