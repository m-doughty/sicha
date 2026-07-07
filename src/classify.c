/* Retry-policy resolution, per-round-trip error classification, and
 * backoff arithmetic.  Everything here is pure and total: any input
 * combination yields a defined answer, so the whole module is
 * table-testable and safe under fuzzed inputs.
 *
 * The classification scheme is ported from the Raku
 * LLM::Data::Inference::Task engine:
 *   ABORT       auth/config errors (400/401/402/403/404),
 *   RETRY_SAME  transient network trouble and anything unclassifiable,
 *   ADVANCE     model/provider pathology more attempts rarely fix.   */

#include <string.h>

#include "sicha_internal.h"

void sicha_policy_resolve(sicha_retry_policy *p)
{
	if (p->max_tries == 0) {
		p->max_tries = SICHA_DEFAULT_MAX_TRIES;
	}
	if (p->backoff_base_ms == 0) {
		p->backoff_base_ms = SICHA_DEFAULT_BACKOFF_BASE_MS;
	}
	if (p->backoff_cap_ms == 0) {
		p->backoff_cap_ms = SICHA_DEFAULT_BACKOFF_CAP_MS;
	}
	if (p->backoff_jitter_ms == 0) {
		p->backoff_jitter_ms = SICHA_DEFAULT_BACKOFF_JITTER_MS;
	}
	if (p->retry_after_cap_ms == 0) {
		p->retry_after_cap_ms = SICHA_DEFAULT_RETRY_AFTER_CAP_MS;
	}
	/* validation_retries: 0 IS the default */
}

sicha_status sicha_policy_validate(const sicha_retry_policy *p)
{
	const uint32_t known = SICHA_POLICY_LENGTH_IS_ADVANCE |
		SICHA_POLICY_CONTENT_FILTER_IS_ADVANCE |
		SICHA_POLICY_RETRY_AFTER_DELTAS;

	if ((p->flags & ~known) != 0) {
		return SICHA_E_INVALID_ARG;
	}
	if (p->n_overrides > 0 && p->overrides == NULL) {
		return SICHA_E_INVALID_ARG;
	}
	for (size_t i = 0; i < p->n_overrides; i++) {
		sicha_error_class c = p->overrides[i].cls;

		if (p->overrides[i].http_status <= 0) {
			return SICHA_E_INVALID_ARG;
		}
		if (c != SICHA_CLASS_ABORT && c != SICHA_CLASS_RETRY_SAME &&
			c != SICHA_CLASS_ADVANCE) {
			return SICHA_E_INVALID_ARG;
		}
	}
	return SICHA_OK;
}

/* First matching override wins. */
static int lookup_override(const sicha_retry_policy *p,
	int32_t http_status, sicha_error_class *out)
{
	for (size_t i = 0; i < p->n_overrides; i++) {
		if (p->overrides[i].http_status == http_status) {
			*out = p->overrides[i].cls;
			return 1;
		}
	}
	return 0;
}

sicha_error_class sicha_classify(const sicha_retry_policy *resolved,
	sicha_transport_status ts, int32_t http_status, int32_t body_state,
	sicha_finish_reason finish)
{
	switch (ts) {
	case SICHA_T_E_CANCELLED:
	case SICHA_T_E_ABORTED_BY_SINK:
		/* sink aborts come from user callbacks returning nonzero */
		return SICHA_CLASS_CANCELLED;
	case SICHA_T_E_TIMEOUT_CONNECT:
	case SICHA_T_E_TIMEOUT_FIRST_BYTE:
	case SICHA_T_E_TIMEOUT_IDLE:
	case SICHA_T_E_TIMEOUT_TOTAL:
		/* client-side timeout: model-specific pathology */
		return SICHA_CLASS_ADVANCE;
	case SICHA_T_E_CONNECT:
	case SICHA_T_E_TLS:
	case SICHA_T_E_RESET:
	case SICHA_T_E_OTHER:
		/* connection-ish / transient / unclassifiable */
		return SICHA_CLASS_RETRY_SAME;
	case SICHA_T_E_PROTOCOL:
		return SICHA_CLASS_ADVANCE;
	case SICHA_T_E_NOMEM:
		/* the engine intercepts NOMEM as terminal before asking */
		return SICHA_CLASS_ADVANCE;
	case SICHA_T_OK:
		break;
	default:
		/* unknown transport status: unclassifiable */
		return SICHA_CLASS_RETRY_SAME;
	}

	if (http_status >= 200 && http_status <= 299) {
		switch (body_state) {
		case SICHA_BODY_EMPTY:
		case SICHA_BODY_MALFORMED:
		case SICHA_BODY_TOOBIG:
			return SICHA_CLASS_ADVANCE;
		default:
			break;
		}
		if (finish == SICHA_FINISH_LENGTH &&
			(resolved->flags & SICHA_POLICY_LENGTH_IS_ADVANCE)) {
			return SICHA_CLASS_ADVANCE;
		}
		if (finish == SICHA_FINISH_CONTENT_FILTER &&
			(resolved->flags &
				SICHA_POLICY_CONTENT_FILTER_IS_ADVANCE)) {
			return SICHA_CLASS_ADVANCE;
		}
		return SICHA_CLASS_NONE;
	}

	{
		sicha_error_class o;

		if (lookup_override(resolved, http_status, &o)) {
			return o;
		}
	}

	switch (http_status) {
	case 400:
	case 401:
	case 402:
	case 403:
	case 404:
		return SICHA_CLASS_ABORT;
	case 429:
		return SICHA_CLASS_ADVANCE;
	default:
		break;
	}
	if (http_status >= 500 && http_status <= 599) {
		return SICHA_CLASS_RETRY_SAME;
	}
	/* 1xx / 3xx / remaining 4xx / anything weird (including a
	 * transport that reported OK with status 0): unclassifiable,
	 * hence retry-same, faithful to the ported engine.  Override
	 * per status if your deployment knows better. */
	return SICHA_CLASS_RETRY_SAME;
}

#define RETRY_AFTER_SAT_MS ((uint64_t)1000 * 1000 * 1000 * 1000 * 1000)

uint64_t sicha_retry_after_ms(const char *value, size_t len)
{
	size_t i = 0;
	uint64_t seconds = 0;
	int digits = 0;

	if (value == NULL) {
		return UINT64_MAX;
	}
	if (len == SICHA_LEN_CSTR) {
		len = strlen(value);
	}
	while (i < len && (value[i] == ' ' || value[i] == '\t')) {
		i++;
	}
	for (; i < len && value[i] >= '0' && value[i] <= '9'; i++) {
		digits++;
		if (seconds > RETRY_AFTER_SAT_MS) {
			seconds = RETRY_AFTER_SAT_MS; /* keep saturated */
		} else {
			seconds = seconds * 10 + (uint64_t)(value[i] - '0');
		}
	}
	while (i < len && (value[i] == ' ' || value[i] == '\t')) {
		i++;
	}
	if (digits == 0 || i != len) {
		/* empty, HTTP-date, or trailing garbage */
		return UINT64_MAX;
	}
	if (seconds > RETRY_AFTER_SAT_MS / 1000) {
		return RETRY_AFTER_SAT_MS;
	}
	return seconds * 1000;
}

uint64_t sicha_backoff_ms(const sicha_retry_policy *resolved,
	uint32_t k, uint64_t jitter_rand)
{
	uint64_t base = resolved->backoff_base_ms;
	uint64_t cap = resolved->backoff_cap_ms;
	uint64_t wait;

	if (k == 0) {
		k = 1;
	}
	if (k - 1 >= 63 || base > (UINT64_MAX >> (k - 1))) {
		wait = cap;
	} else {
		wait = base << (k - 1);
	}
	if (resolved->backoff_jitter_ms != SICHA_DISABLED &&
		resolved->backoff_jitter_ms > 0) {
		uint64_t j = jitter_rand % resolved->backoff_jitter_ms;

		wait = wait > UINT64_MAX - j ? UINT64_MAX : wait + j;
	}
	return wait < cap ? wait : cap;
}
