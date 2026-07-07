/* classify.c: policy resolution/validation, the full classification
 * table (transport statuses, HTTP statuses, body states, finish-
 * reason policies, overrides), Retry-After parsing, and backoff
 * arithmetic against the closed form. */

#include "sicha_internal.h"
#include "support.h"

static sicha_retry_policy resolved_defaults(void)
{
	sicha_retry_policy p;

	memset(&p, 0, sizeof(p));
	sicha_policy_resolve(&p);
	return p;
}

static void check_policy_resolve(void)
{
	sicha_retry_policy p = resolved_defaults();

	T_CHECK(p.max_tries == 3);
	T_CHECK(p.validation_retries == 0);
	T_CHECK(p.backoff_base_ms == 1000);
	T_CHECK(p.backoff_cap_ms == 30000);
	T_CHECK(p.backoff_jitter_ms == 500);
	T_CHECK(p.retry_after_cap_ms == 120000);

	/* explicit values survive */
	memset(&p, 0, sizeof(p));
	p.max_tries = 1;
	p.backoff_base_ms = 10;
	p.backoff_jitter_ms = SICHA_DISABLED;
	p.retry_after_cap_ms = SICHA_DISABLED;
	sicha_policy_resolve(&p);
	T_CHECK(p.max_tries == 1);
	T_CHECK(p.backoff_base_ms == 10);
	T_CHECK(p.backoff_jitter_ms == SICHA_DISABLED);
	T_CHECK(p.retry_after_cap_ms == SICHA_DISABLED);
}

static void check_policy_validate(void)
{
	sicha_retry_policy p;
	sicha_status_override bad_class = { 429, SICHA_CLASS_NONE };
	sicha_status_override bad_status = { 0, SICHA_CLASS_ADVANCE };
	sicha_status_override good = { 429, SICHA_CLASS_RETRY_SAME };

	memset(&p, 0, sizeof(p));
	T_CHECK(sicha_policy_validate(&p) == SICHA_OK);

	p.flags = SICHA_POLICY_LENGTH_IS_ADVANCE |
		SICHA_POLICY_CONTENT_FILTER_IS_ADVANCE |
		SICHA_POLICY_RETRY_AFTER_DELTAS;
	T_CHECK(sicha_policy_validate(&p) == SICHA_OK);
	p.flags = 1u << 30;
	T_CHECK(sicha_policy_validate(&p) == SICHA_E_INVALID_ARG);
	p.flags = 0;

	p.n_overrides = 1;
	p.overrides = NULL;
	T_CHECK(sicha_policy_validate(&p) == SICHA_E_INVALID_ARG);
	p.overrides = &bad_class;
	T_CHECK(sicha_policy_validate(&p) == SICHA_E_INVALID_ARG);
	p.overrides = &bad_status;
	T_CHECK(sicha_policy_validate(&p) == SICHA_E_INVALID_ARG);
	p.overrides = &good;
	T_CHECK(sicha_policy_validate(&p) == SICHA_OK);
}

static void check_transport_classes(void)
{
	sicha_retry_policy p = resolved_defaults();

	T_CHECK(sicha_classify(&p, SICHA_T_E_CANCELLED, 0, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_CANCELLED);
	T_CHECK(sicha_classify(&p, SICHA_T_E_ABORTED_BY_SINK, 200,
		SICHA_BODY_OK, SICHA_FINISH_UNKNOWN) ==
		SICHA_CLASS_CANCELLED);
	T_CHECK(sicha_classify(&p, SICHA_T_E_TIMEOUT_CONNECT, 0,
		SICHA_BODY_OK, SICHA_FINISH_UNKNOWN) ==
		SICHA_CLASS_ADVANCE);
	T_CHECK(sicha_classify(&p, SICHA_T_E_TIMEOUT_FIRST_BYTE, 0,
		SICHA_BODY_OK, SICHA_FINISH_UNKNOWN) ==
		SICHA_CLASS_ADVANCE);
	T_CHECK(sicha_classify(&p, SICHA_T_E_TIMEOUT_IDLE, 200,
		SICHA_BODY_OK, SICHA_FINISH_UNKNOWN) ==
		SICHA_CLASS_ADVANCE);
	T_CHECK(sicha_classify(&p, SICHA_T_E_TIMEOUT_TOTAL, 200,
		SICHA_BODY_OK, SICHA_FINISH_UNKNOWN) ==
		SICHA_CLASS_ADVANCE);
	T_CHECK(sicha_classify(&p, SICHA_T_E_CONNECT, 0, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_RETRY_SAME);
	T_CHECK(sicha_classify(&p, SICHA_T_E_TLS, 0, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_RETRY_SAME);
	T_CHECK(sicha_classify(&p, SICHA_T_E_RESET, 200, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_RETRY_SAME);
	T_CHECK(sicha_classify(&p, SICHA_T_E_OTHER, 0, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_RETRY_SAME);
	T_CHECK(sicha_classify(&p, SICHA_T_E_PROTOCOL, 0, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_ADVANCE);
	T_CHECK(sicha_classify(&p, SICHA_T_E_NOMEM, 0, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_ADVANCE);
	/* unknown transport status: unclassifiable => retry-same */
	T_CHECK(sicha_classify(&p, -77, 0, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_RETRY_SAME);
}

static void check_http_classes(void)
{
	sicha_retry_policy p = resolved_defaults();
	static const int aborts[] = { 400, 401, 402, 403, 404 };
	static const int retries[] = { 405, 406, 408, 410, 418, 300, 301,
		302, 100, 500, 502, 503, 504, 599, 0, 999 };

	for (size_t i = 0; i < SICHA_COUNTOF(aborts); i++) {
		T_CHECK(sicha_classify(&p, SICHA_T_OK, aborts[i],
			SICHA_BODY_OK, SICHA_FINISH_UNKNOWN) ==
			SICHA_CLASS_ABORT);
	}
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 429, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_ADVANCE);
	for (size_t i = 0; i < SICHA_COUNTOF(retries); i++) {
		T_CHECK(sicha_classify(&p, SICHA_T_OK, retries[i],
			SICHA_BODY_OK, SICHA_FINISH_UNKNOWN) ==
			SICHA_CLASS_RETRY_SAME);
	}
}

static void check_success_and_body_states(void)
{
	sicha_retry_policy p = resolved_defaults();

	T_CHECK(sicha_classify(&p, SICHA_T_OK, 200, SICHA_BODY_OK,
		SICHA_FINISH_STOP) == SICHA_CLASS_NONE);
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 201, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_NONE);
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 200, SICHA_BODY_EMPTY,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_ADVANCE);
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 200, SICHA_BODY_MALFORMED,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_ADVANCE);
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 200, SICHA_BODY_TOOBIG,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_ADVANCE);
}

static void check_finish_reason_policies(void)
{
	sicha_retry_policy p = resolved_defaults();

	/* default: truncation and filtering are success */
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 200, SICHA_BODY_OK,
		SICHA_FINISH_LENGTH) == SICHA_CLASS_NONE);
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 200, SICHA_BODY_OK,
		SICHA_FINISH_CONTENT_FILTER) == SICHA_CLASS_NONE);
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 200, SICHA_BODY_OK,
		SICHA_FINISH_TOOL_CALLS) == SICHA_CLASS_NONE);

	p.flags = SICHA_POLICY_LENGTH_IS_ADVANCE;
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 200, SICHA_BODY_OK,
		SICHA_FINISH_LENGTH) == SICHA_CLASS_ADVANCE);
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 200, SICHA_BODY_OK,
		SICHA_FINISH_CONTENT_FILTER) == SICHA_CLASS_NONE);

	p.flags = SICHA_POLICY_CONTENT_FILTER_IS_ADVANCE;
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 200, SICHA_BODY_OK,
		SICHA_FINISH_CONTENT_FILTER) == SICHA_CLASS_ADVANCE);
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 200, SICHA_BODY_OK,
		SICHA_FINISH_LENGTH) == SICHA_CLASS_NONE);
}

static void check_overrides(void)
{
	sicha_retry_policy p = resolved_defaults();
	sicha_status_override ov[] = {
		{ 429, SICHA_CLASS_RETRY_SAME },
		{ 400, SICHA_CLASS_ADVANCE },
		{ 503, SICHA_CLASS_ABORT },
		{ 429, SICHA_CLASS_ABORT }, /* duplicate: first wins */
	};

	p.overrides = ov;
	p.n_overrides = SICHA_COUNTOF(ov);
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 429, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_RETRY_SAME);
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 400, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_ADVANCE);
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 503, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_ABORT);
	/* untouched statuses keep the built-in table */
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 401, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_ABORT);
	T_CHECK(sicha_classify(&p, SICHA_T_OK, 500, SICHA_BODY_OK,
		SICHA_FINISH_UNKNOWN) == SICHA_CLASS_RETRY_SAME);
	/* overrides never apply to 2xx */
	{
		sicha_status_override ok_ov = { 200, SICHA_CLASS_ABORT };

		p.overrides = &ok_ov;
		p.n_overrides = 1;
		T_CHECK(sicha_classify(&p, SICHA_T_OK, 200, SICHA_BODY_OK,
			SICHA_FINISH_UNKNOWN) == SICHA_CLASS_NONE);
	}
	/* overrides never apply to transport failures */
	{
		sicha_status_override t_ov = { 503, SICHA_CLASS_ABORT };

		p.overrides = &t_ov;
		p.n_overrides = 1;
		T_CHECK(sicha_classify(&p, SICHA_T_E_RESET, 503,
			SICHA_BODY_OK, SICHA_FINISH_UNKNOWN) ==
			SICHA_CLASS_RETRY_SAME);
	}
}

static void check_retry_after(void)
{
	T_CHECK(sicha_retry_after_ms("5", SICHA_LEN_CSTR) == 5000);
	T_CHECK(sicha_retry_after_ms("0", SICHA_LEN_CSTR) == 0);
	T_CHECK(sicha_retry_after_ms("120", SICHA_LEN_CSTR) == 120000);
	T_CHECK(sicha_retry_after_ms(" 5 ", SICHA_LEN_CSTR) == 5000);
	T_CHECK(sicha_retry_after_ms("\t7\t", SICHA_LEN_CSTR) == 7000);
	T_CHECK(sicha_retry_after_ms("5x", SICHA_LEN_CSTR) == UINT64_MAX);
	T_CHECK(sicha_retry_after_ms("-5", SICHA_LEN_CSTR) == UINT64_MAX);
	T_CHECK(sicha_retry_after_ms("", SICHA_LEN_CSTR) == UINT64_MAX);
	T_CHECK(sicha_retry_after_ms("  ", SICHA_LEN_CSTR) == UINT64_MAX);
	T_CHECK(sicha_retry_after_ms(NULL, 0) == UINT64_MAX);
	T_CHECK(sicha_retry_after_ms("Wed, 21 Oct 2015 07:28:00 GMT",
		SICHA_LEN_CSTR) == UINT64_MAX);
	/* explicit length: only the covered bytes count */
	T_CHECK(sicha_retry_after_ms("15x", 2) == 15000);
	/* saturation instead of overflow */
	T_CHECK(sicha_retry_after_ms("99999999999999999999999999",
		SICHA_LEN_CSTR) == (uint64_t)1000000000000000ull);
	T_CHECK(sicha_retry_after_ms("18446744073709551616",
		SICHA_LEN_CSTR) == (uint64_t)1000000000000000ull);
}

static void check_backoff(void)
{
	sicha_retry_policy p = resolved_defaults();

	/* no jitter: pure exponential, capped at 30s */
	p.backoff_jitter_ms = SICHA_DISABLED;
	T_CHECK(sicha_backoff_ms(&p, 1, 0) == 1000);
	T_CHECK(sicha_backoff_ms(&p, 2, 0) == 2000);
	T_CHECK(sicha_backoff_ms(&p, 3, 0) == 4000);
	T_CHECK(sicha_backoff_ms(&p, 5, 0) == 16000);
	T_CHECK(sicha_backoff_ms(&p, 6, 0) == 30000); /* 32000 capped */
	T_CHECK(sicha_backoff_ms(&p, 60, 0) == 30000);
	T_CHECK(sicha_backoff_ms(&p, 64, 0) == 30000); /* shift guard */
	T_CHECK(sicha_backoff_ms(&p, 0, 0) == 1000);   /* k clamps to 1 */

	/* jitter adds jitter_rand % jitter */
	p.backoff_jitter_ms = 500;
	T_CHECK(sicha_backoff_ms(&p, 1, 0) == 1000);
	T_CHECK(sicha_backoff_ms(&p, 1, 499) == 1499);
	T_CHECK(sicha_backoff_ms(&p, 1, 500) == 1000);
	T_CHECK(sicha_backoff_ms(&p, 1, 1234) == 1234 % 500 + 1000);
	/* jitter can not push past the cap */
	T_CHECK(sicha_backoff_ms(&p, 6, 499) == 30000);

	/* tiny custom base */
	p.backoff_base_ms = 10;
	p.backoff_cap_ms = 100;
	p.backoff_jitter_ms = SICHA_DISABLED;
	T_CHECK(sicha_backoff_ms(&p, 1, 0) == 10);
	T_CHECK(sicha_backoff_ms(&p, 2, 0) == 20);
	T_CHECK(sicha_backoff_ms(&p, 4, 0) == 80);
	T_CHECK(sicha_backoff_ms(&p, 5, 0) == 100);
}

int main(void)
{
	check_policy_resolve();
	check_policy_validate();
	check_transport_classes();
	check_http_classes();
	check_success_and_body_states();
	check_finish_reason_policies();
	check_overrides();
	check_retry_after();
	check_backoff();
	return t_done("test_classify");
}
