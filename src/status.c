/* Version and enum stringification. */

#include "sicha_internal.h"

#define SICHA_STR2_(x) #x
#define SICHA_STR_(x) SICHA_STR2_(x)

uint32_t sicha_version(void)
{
	return ((uint32_t)SICHA_VERSION_MAJOR << 16) |
		((uint32_t)SICHA_VERSION_MINOR << 8) |
		(uint32_t)SICHA_VERSION_PATCH;
}

const char *sicha_version_str(void)
{
	return SICHA_STR_(SICHA_VERSION_MAJOR) "."
		SICHA_STR_(SICHA_VERSION_MINOR) "."
		SICHA_STR_(SICHA_VERSION_PATCH);
}

const char *sicha_status_str(sicha_status s)
{
	switch (s) {
	case SICHA_OK:
		return "ok";
	case SICHA_E_INVALID_ARG:
		return "invalid argument";
	case SICHA_E_NOMEM:
		return "out of memory";
	case SICHA_E_ABORTED:
		return "aborted (non-retryable upstream error)";
	case SICHA_E_EXHAUSTED:
		return "all backends exhausted";
	case SICHA_E_CANCELLED:
		return "cancelled";
	case SICHA_E_STREAM_LOST:
		return "stream lost after partial delivery";
	default:
		return "unknown status";
	}
}

const char *sicha_error_class_str(sicha_error_class c)
{
	switch (c) {
	case SICHA_CLASS_NONE:
		return "none";
	case SICHA_CLASS_ABORT:
		return "abort";
	case SICHA_CLASS_RETRY_SAME:
		return "retry-same";
	case SICHA_CLASS_ADVANCE:
		return "advance";
	case SICHA_CLASS_CANCELLED:
		return "cancelled";
	case SICHA_CLASS_VALIDATION:
		return "validation";
	default:
		return "unknown class";
	}
}

const char *sicha_finish_reason_str(sicha_finish_reason f)
{
	switch (f) {
	case SICHA_FINISH_UNKNOWN:
		return "unknown";
	case SICHA_FINISH_STOP:
		return "stop";
	case SICHA_FINISH_LENGTH:
		return "length";
	case SICHA_FINISH_TOOL_CALLS:
		return "tool_calls";
	case SICHA_FINISH_CONTENT_FILTER:
		return "content_filter";
	default:
		return "unknown";
	}
}

const char *sicha_transport_status_str(sicha_transport_status s)
{
	switch (s) {
	case SICHA_T_OK:
		return "ok";
	case SICHA_T_E_CONNECT:
		return "connection failed";
	case SICHA_T_E_TLS:
		return "TLS failure";
	case SICHA_T_E_TIMEOUT_CONNECT:
		return "connect timeout";
	case SICHA_T_E_TIMEOUT_FIRST_BYTE:
		return "first-byte timeout";
	case SICHA_T_E_TIMEOUT_IDLE:
		return "idle timeout";
	case SICHA_T_E_TIMEOUT_TOTAL:
		return "total timeout";
	case SICHA_T_E_RESET:
		return "connection reset";
	case SICHA_T_E_ABORTED_BY_SINK:
		return "aborted by sink";
	case SICHA_T_E_CANCELLED:
		return "cancelled";
	case SICHA_T_E_PROTOCOL:
		return "protocol error";
	case SICHA_T_E_NOMEM:
		return "out of memory";
	case SICHA_T_E_OTHER:
		return "transport error";
	default:
		return "unknown transport status";
	}
}
