/* request.c: UTF-8 validation, URL rules, header validation and
 * assembly (injection rejection), backend resolution, and golden
 * JSON bodies including escaping edge cases and extra-body merge
 * precedence. */

#include <stdlib.h>

#include "sicha_internal.h"
#include "support.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static sicha_backend_desc desc_minimal(void)
{
	sicha_backend_desc d;

	memset(&d, 0, sizeof(d));
	d.struct_size = (uint32_t)sizeof(d);
	d.base_url = "http://localhost:5001/v1";
	d.model = "test-model";
	return d;
}

static sicha_request req_minimal(const sicha_message *msgs, size_t n)
{
	sicha_request r;

	memset(&r, 0, sizeof(r));
	r.struct_size = (uint32_t)sizeof(r);
	r.messages = msgs;
	r.n_messages = n;
	return r;
}

static const sicha_message MSG_HI = { SICHA_ROLE_USER, "hi",
	SICHA_LEN_CSTR, NULL };

/* Build against a desc; returns malloc'd JSON or NULL (status out). */
static char *build(const sicha_backend_desc *d, const sicha_request *r,
	int stream, sicha_status *st)
{
	sicha_backend b;
	char *out = NULL;
	size_t out_len = 0;

	*st = sicha_backend_init(&b, d);
	if (*st != SICHA_OK) {
		return NULL;
	}
	*st = sicha_build_chat_body(&b, r, stream, &out, &out_len);
	sicha_backend_free(&b);
	if (*st == SICHA_OK) {
		T_CHECK(out != NULL);
		T_CHECK(strlen(out) == out_len);
	}
	return out;
}

static void golden(const char *name, const sicha_backend_desc *d,
	const sicha_request *r, int stream, const char *expect)
{
	sicha_status st;
	char *json = build(d, r, stream, &st);

	t_check_impl(st == SICHA_OK, name, __FILE__, __LINE__);
	if (json != NULL) {
		t_check_impl(strcmp(json, expect) == 0, name, __FILE__,
			__LINE__);
		if (strcmp(json, expect) != 0) {
			fprintf(stderr, "  got:    %s\n  expect: %s\n",
				json, expect);
		}
		free(json);
	}
}

static void expect_invalid(const char *name, const sicha_backend_desc *d,
	const sicha_request *r)
{
	sicha_status st;
	char *json = build(d, r, 0, &st);

	t_check_impl(st == SICHA_E_INVALID_ARG && json == NULL, name,
		__FILE__, __LINE__);
	free(json);
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

static void check_utf8(void)
{
	T_CHECK(sicha_utf8_valid("", 0));
	T_CHECK(sicha_utf8_valid(NULL, 0));
	T_CHECK(!sicha_utf8_valid(NULL, 1));
	T_CHECK(sicha_utf8_valid("plain ascii", 11));
	T_CHECK(sicha_utf8_valid("caf\xC3\xA9", 5));
	T_CHECK(sicha_utf8_valid("\xE2\x98\x83", 3));         /* ☃ */
	T_CHECK(sicha_utf8_valid("\xF0\x9F\x98\x80", 4));     /* 😀 */
	T_CHECK(sicha_utf8_valid("a\x00" "b", 3));            /* NUL ok */
	T_CHECK(!sicha_utf8_valid("\x80", 1));            /* bare cont */
	T_CHECK(!sicha_utf8_valid("\xC3", 1));            /* truncated */
	T_CHECK(!sicha_utf8_valid("\xC0\xAF", 2));        /* overlong */
	T_CHECK(!sicha_utf8_valid("\xC1\xBF", 2));        /* overlong */
	T_CHECK(!sicha_utf8_valid("\xE0\x80\xAF", 3));    /* overlong */
	T_CHECK(!sicha_utf8_valid("\xED\xA0\x80", 3));    /* surrogate */
	T_CHECK(sicha_utf8_valid("\xED\x9F\xBF", 3));     /* U+D7FF ok */
	T_CHECK(!sicha_utf8_valid("\xF0\x80\x80\x80", 4)); /* overlong */
	T_CHECK(!sicha_utf8_valid("\xF4\x90\x80\x80", 4)); /* >10FFFF */
	T_CHECK(sicha_utf8_valid("\xF4\x8F\xBF\xBF", 4)); /* U+10FFFF */
	T_CHECK(!sicha_utf8_valid("\xF5\x80\x80\x80", 4));
	T_CHECK(!sicha_utf8_valid("\xFF", 1));
	T_CHECK(!sicha_utf8_valid("ok\xC3", 3));   /* truncated at end */
}

/* ------------------------------------------------------------------ */
/* URLs                                                                */
/* ------------------------------------------------------------------ */

static void check_urls(void)
{
	T_CHECK(sicha_url_validate("http://localhost:5001/v1") ==
		SICHA_OK);
	T_CHECK(sicha_url_validate("https://openrouter.ai/api/v1") ==
		SICHA_OK);
	T_CHECK(sicha_url_validate("HTTPS://Host/v1") == SICHA_OK);
	T_CHECK(sicha_url_validate("http://h") == SICHA_OK);
	T_CHECK(sicha_url_validate(NULL) == SICHA_E_INVALID_ARG);
	T_CHECK(sicha_url_validate("") == SICHA_E_INVALID_ARG);
	T_CHECK(sicha_url_validate("localhost:5001") ==
		SICHA_E_INVALID_ARG);
	T_CHECK(sicha_url_validate("ftp://host") == SICHA_E_INVALID_ARG);
	T_CHECK(sicha_url_validate("http://") == SICHA_E_INVALID_ARG);
	T_CHECK(sicha_url_validate("http:///path") ==
		SICHA_E_INVALID_ARG);
	T_CHECK(sicha_url_validate("http://host /v1") ==
		SICHA_E_INVALID_ARG);
	T_CHECK(sicha_url_validate("http://host\r\nX: y") ==
		SICHA_E_INVALID_ARG);
	T_CHECK(sicha_url_validate("http://host/v1#frag") ==
		SICHA_E_INVALID_ARG);
	T_CHECK(sicha_url_validate("http://host/v1?q=1") ==
		SICHA_E_INVALID_ARG);
	T_CHECK(sicha_url_validate("http://host\\v1") ==
		SICHA_E_INVALID_ARG);

	{
		char *u = sicha_url_join("http://h:1/v1",
			"/chat/completions");

		T_CHECK(u != NULL &&
			strcmp(u, "http://h:1/v1/chat/completions") == 0);
		free(u);
		u = sicha_url_join("http://h:1/v1///", "/chat/completions");
		T_CHECK(u != NULL &&
			strcmp(u, "http://h:1/v1/chat/completions") == 0);
		free(u);
		u = sicha_url_origin("https://h.example:8443/api/v1/x");
		T_CHECK(u != NULL &&
			strcmp(u, "https://h.example:8443") == 0);
		free(u);
		u = sicha_url_origin("http://plain");
		T_CHECK(u != NULL && strcmp(u, "http://plain") == 0);
		free(u);
	}
}

/* ------------------------------------------------------------------ */
/* Headers                                                             */
/* ------------------------------------------------------------------ */

static void check_header_validation(void)
{
	T_CHECK(sicha_header_validate("X-Title", "My App") == SICHA_OK);
	T_CHECK(sicha_header_validate("HTTP-Referer",
		"https://a.example") == SICHA_OK);
	T_CHECK(sicha_header_validate("X-Utf8", "caf\xC3\xA9") ==
		SICHA_OK);
	T_CHECK(sicha_header_validate("X-Tab", "a\tb") == SICHA_OK);
	T_CHECK(sicha_header_validate(NULL, "v") == SICHA_E_INVALID_ARG);
	T_CHECK(sicha_header_validate("N", NULL) == SICHA_E_INVALID_ARG);
	T_CHECK(sicha_header_validate("", "v") == SICHA_E_INVALID_ARG);
	T_CHECK(sicha_header_validate("Bad Name", "v") ==
		SICHA_E_INVALID_ARG);
	T_CHECK(sicha_header_validate("Bad:Name", "v") ==
		SICHA_E_INVALID_ARG);
	T_CHECK(sicha_header_validate("X", "inject\r\nEvil: 1") ==
		SICHA_E_INVALID_ARG);
	T_CHECK(sicha_header_validate("X", "inject\nEvil") ==
		SICHA_E_INVALID_ARG);
	T_CHECK(sicha_header_validate("X", "del\x7F") ==
		SICHA_E_INVALID_ARG);

	T_CHECK(sicha_header_name_eq("content-type", "Content-Type"));
	T_CHECK(sicha_header_name_eq("ACCEPT", "accept"));
	T_CHECK(!sicha_header_name_eq("Accept", "Accept-Encoding"));
	T_CHECK(!sicha_header_name_eq("Acce", "Accept"));
}

static const char *find_header(const sicha_header *h, size_t n,
	const char *name)
{
	for (size_t i = 0; i < n; i++) {
		if (sicha_header_name_eq(h[i].name, name)) {
			return h[i].value;
		}
	}
	return NULL;
}

static void check_header_assembly(void)
{
	sicha_backend_desc d = desc_minimal();
	sicha_header extras[] = {
		{ "HTTP-Referer", "https://app.example" },
		{ "X-Title", "Cantina" },
	};
	sicha_backend b;
	sicha_header *h = NULL;
	size_t n = 0;

	d.api_key = "sk-secret-123";
	d.extra_headers = extras;
	d.n_extra_headers = SICHA_COUNTOF(extras);
	T_CHECK(sicha_backend_init(&b, &d) == SICHA_OK);

	T_CHECK(sicha_build_headers(&b, 1, "sicha/test", &h, &n) ==
		SICHA_OK);
	T_CHECK(n == 6);
	T_CHECK(find_header(h, n, "Content-Type") != NULL &&
		strcmp(find_header(h, n, "Content-Type"),
			"application/json") == 0);
	T_CHECK(find_header(h, n, "Accept") != NULL &&
		strcmp(find_header(h, n, "Accept"),
			"text/event-stream") == 0);
	T_CHECK(find_header(h, n, "User-Agent") != NULL &&
		strcmp(find_header(h, n, "User-Agent"), "sicha/test") == 0);
	T_CHECK(find_header(h, n, "Authorization") != NULL &&
		strcmp(find_header(h, n, "Authorization"),
			"Bearer sk-secret-123") == 0);
	T_CHECK(find_header(h, n, "X-Title") != NULL &&
		strcmp(find_header(h, n, "X-Title"), "Cantina") == 0);
	sicha_headers_free(h, n);

	/* non-streaming accept; no api_key drops Authorization */
	sicha_backend_free(&b);
	d.api_key = NULL;
	T_CHECK(sicha_backend_init(&b, &d) == SICHA_OK);
	T_CHECK(sicha_build_headers(&b, 0, "ua", &h, &n) == SICHA_OK);
	T_CHECK(n == 5);
	T_CHECK(find_header(h, n, "Accept") != NULL &&
		strcmp(find_header(h, n, "Accept"),
			"application/json") == 0);
	T_CHECK(find_header(h, n, "Authorization") == NULL);
	sicha_headers_free(h, n);
	sicha_backend_free(&b);
}

/* ------------------------------------------------------------------ */
/* Backend resolution                                                  */
/* ------------------------------------------------------------------ */

static void check_backend_init(void)
{
	sicha_backend b;
	sicha_backend_desc d = desc_minimal();

	/* happy path resolves urls and timeouts */
	T_CHECK(sicha_backend_init(&b, &d) == SICHA_OK);
	T_CHECK(strcmp(b.url_chat,
		"http://localhost:5001/v1/chat/completions") == 0);
	T_CHECK(b.url_abort == NULL);
	T_CHECK(b.timeouts.connect_ms == SICHA_DEFAULT_CONNECT_MS);
	T_CHECK(b.timeouts.first_byte_ms == SICHA_DEFAULT_FIRST_BYTE_MS);
	T_CHECK(b.timeouts.idle_ms == SICHA_DEFAULT_IDLE_MS);
	T_CHECK(b.timeouts.total_ms == SICHA_DEFAULT_TOTAL_MS);
	sicha_backend_free(&b);

	/* kobold assist derives the abort url from the ORIGIN */
	d.flags = SICHA_BACKEND_KOBOLD_CANCEL_ASSIST;
	T_CHECK(sicha_backend_init(&b, &d) == SICHA_OK);
	T_CHECK(b.url_abort != NULL && strcmp(b.url_abort,
		"http://localhost:5001/api/extra/abort") == 0);
	sicha_backend_free(&b);
	d.flags = 0;

	/* explicit timeouts survive, INFINITE included */
	d.timeouts.first_byte_ms = SICHA_INFINITE;
	d.timeouts.connect_ms = 5;
	T_CHECK(sicha_backend_init(&b, &d) == SICHA_OK);
	T_CHECK(b.timeouts.first_byte_ms == SICHA_INFINITE);
	T_CHECK(b.timeouts.connect_ms == 5);
	sicha_backend_free(&b);
	memset(&d.timeouts, 0, sizeof(d.timeouts));

	/* rejections */
	{
		sicha_backend_desc bad = desc_minimal();

		bad.struct_size = 4;
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
		bad = desc_minimal();
		bad.base_url = "not-a-url";
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
		bad = desc_minimal();
		bad.model = NULL;
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
		bad = desc_minimal();
		bad.model = "";
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
		bad = desc_minimal();
		bad.model = "bad\xFFutf8";
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
		bad = desc_minimal();
		bad.api_key = "evil\r\nX-Injected: 1";
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
		bad = desc_minimal();
		bad.api_key = "";
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
		bad = desc_minimal();
		bad.flags = 1u << 20;
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
		bad = desc_minimal();
		bad.extra_body_json = "[1,2]"; /* array, not object */
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
		bad = desc_minimal();
		bad.extra_body_json = "{broken";
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
	}
	/* reserved / colliding extra headers */
	{
		sicha_backend_desc bad = desc_minimal();
		sicha_header hct = { "Content-Type", "text/plain" };
		sicha_header hauth = { "authorization", "Bearer x" };
		sicha_header hhost = { "Host", "evil" };
		sicha_header hinj = { "X-Ok", "v\r\nEvil: 1" };

		bad.extra_headers = &hct;
		bad.n_extra_headers = 1;
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
		bad.extra_headers = &hhost;
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
		bad.extra_headers = &hinj;
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
		/* Authorization collides only when an api_key is set */
		bad.extra_headers = &hauth;
		T_CHECK(sicha_backend_init(&b, &bad) == SICHA_OK);
		sicha_backend_free(&b);
		bad.api_key = "sk-1";
		T_CHECK(sicha_backend_init(&b, &bad) ==
			SICHA_E_INVALID_ARG);
	}
}

/* ------------------------------------------------------------------ */
/* Golden bodies                                                       */
/* ------------------------------------------------------------------ */

static void check_golden_bodies(void)
{
	sicha_backend_desc d = desc_minimal();

	{
		sicha_request r = req_minimal(&MSG_HI, 1);

		golden("minimal", &d, &r, 0,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":\"hi\"}]}");
		golden("minimal stream", &d, &r, 1,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":\"hi\"}],"
			"\"stream\":true}");
	}
	{
		sicha_message msgs[] = {
			{ SICHA_ROLE_SYSTEM, "be brief", SICHA_LEN_CSTR,
				NULL },
			{ SICHA_ROLE_USER, "hi", SICHA_LEN_CSTR, NULL },
			{ SICHA_ROLE_ASSISTANT, "hello", SICHA_LEN_CSTR,
				NULL },
			{ SICHA_ROLE_TOOL, "42", SICHA_LEN_CSTR,
				"call_9" },
		};
		sicha_request r = req_minimal(msgs, SICHA_COUNTOF(msgs));

		golden("all roles", &d, &r, 0,
			"{\"model\":\"test-model\",\"messages\":["
			"{\"role\":\"system\",\"content\":\"be brief\"},"
			"{\"role\":\"user\",\"content\":\"hi\"},"
			"{\"role\":\"assistant\",\"content\":\"hello\"},"
			"{\"role\":\"tool\",\"content\":\"42\","
			"\"tool_call_id\":\"call_9\"}]}");
	}
	{
		sicha_request r = req_minimal(&MSG_HI, 1);
		static const char *stops[] = { "\n\n", "<|end|>" };

		r.set_mask = SICHA_SET_MAX_TOKENS | SICHA_SET_TEMPERATURE |
			SICHA_SET_TOP_P | SICHA_SET_TOP_K |
			SICHA_SET_PRESENCE_PENALTY |
			SICHA_SET_FREQUENCY_PENALTY;
		r.max_tokens = 256;
		r.temperature = 0.7;
		r.top_p = 0.95;
		r.top_k = 40;
		r.presence_penalty = 0.25;
		r.frequency_penalty = -0.5;
		r.stop = stops;
		r.n_stop = 2;
		golden("full samplers", &d, &r, 0,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":\"hi\"}],"
			"\"max_tokens\":256,\"temperature\":0.7,"
			"\"top_p\":0.95,\"top_k\":40,"
			"\"presence_penalty\":0.25,"
			"\"frequency_penalty\":-0.5,"
			"\"stop\":[\"\\n\\n\",\"<|end|>\"]}");
	}
	{
		/* zeroed set_mask sends NO samplers even when the fields
		 * hold values */
		sicha_request r = req_minimal(&MSG_HI, 1);

		r.max_tokens = 999;
		r.temperature = 2.0;
		golden("unset samplers omitted", &d, &r, 0,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":\"hi\"}]}");
	}
	{
		sicha_request r = req_minimal(&MSG_HI, 1);

		r.response_format_json = "{\"type\":\"json_schema\","
			"\"json_schema\":{\"name\":\"r\",\"schema\":"
			"{\"type\":\"object\"}}}";
		golden("response_format passthrough", &d, &r, 0,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":\"hi\"}],"
			"\"response_format\":{\"type\":\"json_schema\","
			"\"json_schema\":{\"name\":\"r\",\"schema\":"
			"{\"type\":\"object\"}}}}");
	}
	{
		/* messages_json passthrough: content-parts shape v0.1
		 * does not model */
		sicha_request r;

		memset(&r, 0, sizeof(r));
		r.struct_size = (uint32_t)sizeof(r);
		r.messages_json = "[{\"role\":\"user\",\"content\":"
			"[{\"type\":\"text\",\"text\":\"look\"}]}]";
		golden("messages_json passthrough", &d, &r, 0,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":"
			"[{\"type\":\"text\",\"text\":\"look\"}]}]}");
	}
	{
		/* extra body: adds provider fields, overrides generated
		 * ones (temperature), and can even override model */
		sicha_backend_desc kd = desc_minimal();
		sicha_request r = req_minimal(&MSG_HI, 1);

		kd.extra_body_json = "{\"min_p\":0.05,\"temperature\":1.1}";
		r.set_mask = SICHA_SET_TEMPERATURE;
		r.temperature = 0.7;
		golden("extra body wins collisions", &kd, &r, 0,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":\"hi\"}],"
			"\"min_p\":0.05,\"temperature\":1.1}");

		kd.extra_body_json = "{\"model\":\"other\"}";
		r.set_mask = 0;
		golden("extra body can override model", &kd, &r, 0,
			"{\"messages\":[{\"role\":\"user\","
			"\"content\":\"hi\"}],\"model\":\"other\"}");
	}
	{
		/* stream usage flag */
		sicha_backend_desc sd = desc_minimal();
		sicha_request r = req_minimal(&MSG_HI, 1);

		sd.flags = SICHA_BACKEND_STREAM_USAGE;
		golden("stream_options on flag", &sd, &r, 1,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":\"hi\"}],"
			"\"stream\":true,\"stream_options\":"
			"{\"include_usage\":true}}");
		/* ...but not on non-streaming calls */
		golden("no stream_options when not streaming", &sd, &r, 0,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":\"hi\"}]}");
	}
}

static void check_escaping(void)
{
	sicha_backend_desc d = desc_minimal();

	{
		sicha_message m = { SICHA_ROLE_USER,
			"quote\" back\\slash\ttab\nnl", SICHA_LEN_CSTR,
			NULL };
		sicha_request r = req_minimal(&m, 1);

		golden("escapes", &d, &r, 0,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":"
			"\"quote\\\" back\\\\slash\\ttab\\nnl\"}]}");
	}
	{
		sicha_message m = { SICHA_ROLE_USER, "\x01\x1F", 2, NULL };
		sicha_request r = req_minimal(&m, 1);

		golden("control chars", &d, &r, 0,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":"
			"\"\\u0001\\u001F\"}]}");
	}
	{
		/* emoji passes through raw (yyjson writes UTF-8) */
		sicha_message m = { SICHA_ROLE_USER, "hi \xF0\x9F\x98\x80",
			SICHA_LEN_CSTR, NULL };
		sicha_request r = req_minimal(&m, 1);

		golden("emoji", &d, &r, 0,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":"
			"\"hi \xF0\x9F\x98\x80\"}]}");
	}
	{
		/* embedded NUL via explicit length */
		sicha_message m = { SICHA_ROLE_USER, "a\x00" "b", 3, NULL };
		sicha_request r = req_minimal(&m, 1);

		golden("embedded NUL", &d, &r, 0,
			"{\"model\":\"test-model\",\"messages\":"
			"[{\"role\":\"user\",\"content\":"
			"\"a\\u0000b\"}]}");
	}
}

static void check_request_rejections(void)
{
	sicha_backend_desc d = desc_minimal();

	{
		sicha_request r = req_minimal(&MSG_HI, 1);

		r.struct_size = 8;
		expect_invalid("bad struct_size", &d, &r);
	}
	{
		sicha_request r = req_minimal(NULL, 0);

		expect_invalid("no messages at all", &d, &r);
	}
	{
		sicha_request r = req_minimal(&MSG_HI, 0);

		expect_invalid("messages with zero count", &d, &r);
	}
	{
		sicha_request r = req_minimal(&MSG_HI, 1);

		r.messages_json = "[]";
		expect_invalid("both message sources", &d, &r);
	}
	{
		sicha_request r = req_minimal(NULL, 0);

		r.messages_json = "{\"not\":\"array\"}";
		expect_invalid("messages_json not an array", &d, &r);
		r.messages_json = "[broken";
		expect_invalid("messages_json malformed", &d, &r);
	}
	{
		sicha_message m = { 99, "hi", SICHA_LEN_CSTR, NULL };
		sicha_request r = req_minimal(&m, 1);

		expect_invalid("bad role", &d, &r);
	}
	{
		sicha_message m = { SICHA_ROLE_USER, NULL, 0, NULL };
		sicha_request r = req_minimal(&m, 1);

		expect_invalid("NULL content", &d, &r);
	}
	{
		sicha_message m = { SICHA_ROLE_USER, "bad\xFF", 4, NULL };
		sicha_request r = req_minimal(&m, 1);

		expect_invalid("invalid UTF-8 content", &d, &r);
	}
	{
		sicha_message m = { SICHA_ROLE_USER, "hi", SICHA_LEN_CSTR,
			"call_1" };
		sicha_request r = req_minimal(&m, 1);

		expect_invalid("tool_call_id on non-tool role", &d, &r);
	}
	{
		sicha_request r = req_minimal(&MSG_HI, 1);

		r.set_mask = 1u << 20;
		expect_invalid("unknown set bit", &d, &r);
	}
	{
		sicha_request r = req_minimal(&MSG_HI, 1);

		r.flags = 1u << 20;
		expect_invalid("unknown request flag", &d, &r);
	}
	{
		sicha_request r = req_minimal(&MSG_HI, 1);

		r.set_mask = SICHA_SET_MAX_TOKENS;
		r.max_tokens = 0;
		expect_invalid("max_tokens zero", &d, &r);
	}
	{
		sicha_request r = req_minimal(&MSG_HI, 1);

		r.set_mask = SICHA_SET_TEMPERATURE;
		r.temperature = -1.0;
		expect_invalid("negative temperature", &d, &r);
		r.temperature = 0.0 / 0.0; /* NaN through FFI */
		expect_invalid("NaN temperature", &d, &r);
	}
	{
		sicha_request r = req_minimal(&MSG_HI, 1);
		static const char *bad_stop[] = { "" };

		r.stop = bad_stop;
		r.n_stop = 1;
		expect_invalid("empty stop sequence", &d, &r);
		r.stop = NULL;
		expect_invalid("stop NULL with count", &d, &r);
	}
	{
		sicha_request r = req_minimal(&MSG_HI, 1);

		r.response_format_json = "[1]";
		expect_invalid("response_format not object", &d, &r);
		r.response_format_json = "{bad";
		expect_invalid("response_format malformed", &d, &r);
	}
}

int main(void)
{
	check_utf8();
	check_urls();
	check_header_validation();
	check_header_assembly();
	check_backend_init();
	check_golden_bodies();
	check_escaping();
	check_request_rejections();
	return t_done("test_request");
}
