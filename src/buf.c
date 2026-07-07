/* Growable byte buffer with overflow-checked growth, an optional hard
 * length cap, sticky errors, and a permanent NUL-termination
 * invariant (data[len] == '\0' whenever data != NULL). */

#include <stdlib.h>
#include <string.h>

#include "sicha_internal.h"

void sicha_buf_init(sicha_buf *b, size_t max_len)
{
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
	b->max_len = max_len;
	b->err = SICHA_OK;
}

void sicha_buf_free(sicha_buf *b)
{
	free(b->data);
	sicha_buf_init(b, b->max_len);
}

sicha_status sicha_buf_status(const sicha_buf *b)
{
	return b->err;
}

/* Ensure room for `need` MORE bytes plus the NUL slot. */
static int buf_reserve(sicha_buf *b, size_t need)
{
	size_t want;
	size_t cap;
	char *p;

	if (b->err != SICHA_OK) {
		return 0;
	}
	if (need > SIZE_MAX - b->len) {
		b->err = SICHA_E_NOMEM; /* length arithmetic overflow */
		return 0;
	}
	want = b->len + need;
	if (b->max_len != 0 && want > b->max_len) {
		b->err = SICHA_INT_E_TOOBIG;
		return 0;
	}
	if (b->data != NULL && want <= b->cap) {
		return 1;
	}
	cap = b->cap == 0 ? 64 : b->cap;
	while (cap < want) {
		if (cap > SIZE_MAX / 2) {
			cap = want;
			break;
		}
		cap *= 2;
	}
	if (cap == SIZE_MAX) {
		b->err = SICHA_E_NOMEM; /* no room for the NUL slot */
		return 0;
	}
	p = realloc(b->data, cap + 1);
	if (p == NULL) {
		b->err = SICHA_E_NOMEM;
		return 0;
	}
	b->data = p;
	b->cap = cap;
	b->data[b->len] = '\0';
	return 1;
}

void sicha_buf_append(sicha_buf *b, const char *bytes, size_t len)
{
	if (len == 0) {
		/* still materialize the NUL invariant for empty buffers */
		(void)buf_reserve(b, 0);
		return;
	}
	if (!buf_reserve(b, len)) {
		return;
	}
	memcpy(b->data + b->len, bytes, len);
	b->len += len;
	b->data[b->len] = '\0';
}

void sicha_buf_append_cstr(sicha_buf *b, const char *s)
{
	sicha_buf_append(b, s, strlen(s));
}

void sicha_buf_append_ch(sicha_buf *b, char ch)
{
	sicha_buf_append(b, &ch, 1);
}

void sicha_buf_reset(sicha_buf *b)
{
	b->len = 0;
	if (b->data != NULL) {
		b->data[0] = '\0';
	}
}

char *sicha_buf_take(sicha_buf *b, size_t *len)
{
	char *out;

	if (b->err != SICHA_OK) {
		sicha_buf_free(b);
		if (len != NULL) {
			*len = 0;
		}
		return NULL;
	}
	if (b->data == NULL && !buf_reserve(b, 0)) {
		if (len != NULL) {
			*len = 0;
		}
		return NULL;
	}
	out = b->data;
	if (len != NULL) {
		*len = b->len;
	}
	b->data = NULL;
	sicha_buf_init(b, b->max_len);
	return out;
}
