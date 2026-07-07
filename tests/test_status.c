/* Version and stringifier coverage: every enum value maps to a
 * distinct, stable string; out-of-range values hit the fallback. */

#include "support.h"

static void check_version(void)
{
	uint32_t v = sicha_version();

	T_CHECK(v == (((uint32_t)SICHA_VERSION_MAJOR << 16) |
		((uint32_t)SICHA_VERSION_MINOR << 8) |
		(uint32_t)SICHA_VERSION_PATCH));
	T_CHECK((v >> 16) == SICHA_VERSION_MAJOR);
	T_CHECK(((v >> 8) & 0xFF) == SICHA_VERSION_MINOR);
	T_CHECK((v & 0xFF) == SICHA_VERSION_PATCH);
	{
		char buf[32];

		snprintf(buf, sizeof(buf), "%d.%d.%d", SICHA_VERSION_MAJOR,
			SICHA_VERSION_MINOR, SICHA_VERSION_PATCH);
		T_CHECK(strcmp(sicha_version_str(), buf) == 0);
	}
}

static void check_status_str(void)
{
	static const sicha_status all[] = { SICHA_OK, SICHA_E_INVALID_ARG,
		SICHA_E_NOMEM, SICHA_E_ABORTED, SICHA_E_EXHAUSTED,
		SICHA_E_CANCELLED, SICHA_E_STREAM_LOST };

	T_CHECK(strcmp(sicha_status_str(SICHA_OK), "ok") == 0);
	T_CHECK(strcmp(sicha_status_str(SICHA_E_CANCELLED),
		"cancelled") == 0);
	T_CHECK(strcmp(sicha_status_str(-999), "unknown status") == 0);
	T_CHECK(strcmp(sicha_status_str(1), "unknown status") == 0);
	/* distinct and non-NULL across the board */
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		T_CHECK(sicha_status_str(all[i]) != NULL);
		T_CHECK(strcmp(sicha_status_str(all[i]),
			"unknown status") != 0);
		for (size_t j = i + 1; j < sizeof(all) / sizeof(all[0]);
			j++) {
			T_CHECK(strcmp(sicha_status_str(all[i]),
				sicha_status_str(all[j])) != 0);
		}
	}
}

static void check_error_class_str(void)
{
	static const sicha_error_class all[] = { SICHA_CLASS_NONE,
		SICHA_CLASS_ABORT, SICHA_CLASS_RETRY_SAME,
		SICHA_CLASS_ADVANCE, SICHA_CLASS_CANCELLED,
		SICHA_CLASS_VALIDATION };

	T_CHECK(strcmp(sicha_error_class_str(SICHA_CLASS_RETRY_SAME),
		"retry-same") == 0);
	T_CHECK(strcmp(sicha_error_class_str(SICHA_CLASS_ADVANCE),
		"advance") == 0);
	T_CHECK(strcmp(sicha_error_class_str(99), "unknown class") == 0);
	T_CHECK(strcmp(sicha_error_class_str(-1), "unknown class") == 0);
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		for (size_t j = i + 1; j < sizeof(all) / sizeof(all[0]);
			j++) {
			T_CHECK(strcmp(sicha_error_class_str(all[i]),
				sicha_error_class_str(all[j])) != 0);
		}
	}
}

static void check_finish_reason_str(void)
{
	T_CHECK(strcmp(sicha_finish_reason_str(SICHA_FINISH_STOP),
		"stop") == 0);
	T_CHECK(strcmp(sicha_finish_reason_str(SICHA_FINISH_LENGTH),
		"length") == 0);
	T_CHECK(strcmp(sicha_finish_reason_str(SICHA_FINISH_TOOL_CALLS),
		"tool_calls") == 0);
	T_CHECK(strcmp(
		sicha_finish_reason_str(SICHA_FINISH_CONTENT_FILTER),
		"content_filter") == 0);
	T_CHECK(strcmp(sicha_finish_reason_str(SICHA_FINISH_UNKNOWN),
		"unknown") == 0);
	/* out-of-range folds into the unknown fallback */
	T_CHECK(strcmp(sicha_finish_reason_str(77), "unknown") == 0);
}

static void check_transport_status_str(void)
{
	static const sicha_transport_status all[] = { SICHA_T_OK,
		SICHA_T_E_CONNECT, SICHA_T_E_TLS, SICHA_T_E_TIMEOUT_CONNECT,
		SICHA_T_E_TIMEOUT_FIRST_BYTE, SICHA_T_E_TIMEOUT_IDLE,
		SICHA_T_E_TIMEOUT_TOTAL, SICHA_T_E_RESET,
		SICHA_T_E_ABORTED_BY_SINK, SICHA_T_E_CANCELLED,
		SICHA_T_E_PROTOCOL, SICHA_T_E_NOMEM, SICHA_T_E_OTHER };

	T_CHECK(strcmp(sicha_transport_status_str(SICHA_T_E_TIMEOUT_IDLE),
		"idle timeout") == 0);
	T_CHECK(strcmp(sicha_transport_status_str(-999),
		"unknown transport status") == 0);
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		T_CHECK(sicha_transport_status_str(all[i]) != NULL);
		for (size_t j = i + 1; j < sizeof(all) / sizeof(all[0]);
			j++) {
			T_CHECK(strcmp(sicha_transport_status_str(all[i]),
				sicha_transport_status_str(all[j])) != 0);
		}
	}
}

int main(void)
{
	check_version();
	check_status_str();
	check_error_class_str();
	check_finish_reason_str();
	check_transport_status_str();
	return t_done("test_status");
}
