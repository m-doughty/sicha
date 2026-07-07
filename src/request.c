/* Input validation, URL and header assembly, backend resolution, and
 * chat-completion body serialization (yyjson).
 *
 * Everything user-supplied is validated at a boundary: URLs at client
 * create, header names/values against CR/LF/NUL injection, message
 * content against strict UTF-8 (embedded NUL is legal WITH an
 * explicit length — it serializes as \u0000).  The api_key is
 * validated like a header value and only ever lands in the
 * Authorization header, never in a body or a log. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sicha_internal.h"
#include "yyjson.h"

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

int sicha_utf8_valid(const char *bytes, size_t len)
{
	const uint8_t *p = (const uint8_t *)bytes;
	size_t i = 0;

	if (bytes == NULL) {
		return len == 0;
	}
	while (i < len) {
		uint8_t b = p[i];

		if (b < 0x80) {
			i++;
			continue;
		}
		if (b < 0xC2) {
			return 0; /* continuation byte or overlong lead */
		}
		if (b < 0xE0) { /* 2 bytes */
			if (len - i < 2 || (p[i + 1] & 0xC0) != 0x80) {
				return 0;
			}
			i += 2;
			continue;
		}
		if (b < 0xF0) { /* 3 bytes */
			uint8_t lo = 0x80;
			uint8_t hi = 0xBF;

			if (len - i < 3) {
				return 0;
			}
			if (b == 0xE0) {
				lo = 0xA0; /* no overlongs */
			} else if (b == 0xED) {
				hi = 0x9F; /* no surrogates */
			}
			if (p[i + 1] < lo || p[i + 1] > hi ||
				(p[i + 2] & 0xC0) != 0x80) {
				return 0;
			}
			i += 3;
			continue;
		}
		if (b < 0xF5) { /* 4 bytes */
			uint8_t lo = 0x80;
			uint8_t hi = 0xBF;

			if (len - i < 4) {
				return 0;
			}
			if (b == 0xF0) {
				lo = 0x90; /* no overlongs */
			} else if (b == 0xF4) {
				hi = 0x8F; /* <= U+10FFFF */
			}
			if (p[i + 1] < lo || p[i + 1] > hi ||
				(p[i + 2] & 0xC0) != 0x80 ||
				(p[i + 3] & 0xC0) != 0x80) {
				return 0;
			}
			i += 4;
			continue;
		}
		return 0; /* 0xF5..0xFF never appear in UTF-8 */
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* Small string helpers                                                */
/* ------------------------------------------------------------------ */

static char *dup_cstr(const char *s)
{
	size_t n = strlen(s);
	char *p = malloc(n + 1);

	if (p != NULL) {
		memcpy(p, s, n + 1);
	}
	return p;
}

static int prefix_ci(const char *s, const char *prefix)
{
	while (*prefix != '\0') {
		char a = *s;
		char b = *prefix;

		if (a >= 'A' && a <= 'Z') {
			a = (char)(a - 'A' + 'a');
		}
		if (a != b) {
			return 0;
		}
		s++;
		prefix++;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* URLs                                                                */
/* ------------------------------------------------------------------ */

/* Length of the scheme prefix ("http://" or "https://"), 0 if bad. */
static size_t scheme_len(const char *url)
{
	if (prefix_ci(url, "https://")) {
		return 8;
	}
	if (prefix_ci(url, "http://")) {
		return 7;
	}
	return 0;
}

sicha_status sicha_url_validate(const char *base_url)
{
	size_t s;
	size_t i;

	if (base_url == NULL) {
		return SICHA_E_INVALID_ARG;
	}
	s = scheme_len(base_url);
	if (s == 0) {
		return SICHA_E_INVALID_ARG;
	}
	if (base_url[s] == '\0' || base_url[s] == '/') {
		return SICHA_E_INVALID_ARG; /* empty host */
	}
	for (i = 0; base_url[i] != '\0'; i++) {
		uint8_t b = (uint8_t)base_url[i];

		if (b <= 0x20 || b == 0x7F || b == '#' || b == '?' ||
			b == '\\') {
			return SICHA_E_INVALID_ARG;
		}
	}
	return SICHA_OK;
}

char *sicha_url_join(const char *base_url, const char *path)
{
	size_t blen = strlen(base_url);
	size_t plen = strlen(path);
	char *out;

	while (blen > 0 && base_url[blen - 1] == '/') {
		blen--;
	}
	out = malloc(blen + plen + 1);
	if (out == NULL) {
		return NULL;
	}
	memcpy(out, base_url, blen);
	memcpy(out + blen, path, plen + 1);
	return out;
}

char *sicha_url_origin(const char *base_url)
{
	size_t s = scheme_len(base_url);
	size_t end = s;
	char *out;

	while (base_url[end] != '\0' && base_url[end] != '/') {
		end++;
	}
	out = malloc(end + 1);
	if (out == NULL) {
		return NULL;
	}
	memcpy(out, base_url, end);
	out[end] = '\0';
	return out;
}

/* ------------------------------------------------------------------ */
/* Headers                                                             */
/* ------------------------------------------------------------------ */

int sicha_header_name_eq(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0') {
		char ca = *a;
		char cb = *b;

		if (ca >= 'A' && ca <= 'Z') {
			ca = (char)(ca - 'A' + 'a');
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb = (char)(cb - 'A' + 'a');
		}
		if (ca != cb) {
			return 0;
		}
		a++;
		b++;
	}
	return *a == *b;
}

/* RFC 7230 token characters. */
static int is_token_char(uint8_t b)
{
	if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
		(b >= '0' && b <= '9')) {
		return 1;
	}
	switch (b) {
	case '!': case '#': case '$': case '%': case '&': case '\'':
	case '*': case '+': case '-': case '.': case '^': case '_':
	case '`': case '|': case '~':
		return 1;
	default:
		return 0;
	}
}

/* Header VALUE bytes: no C0 controls (tab allowed), no DEL.  Bytes
 * >= 0x80 pass through (some providers take UTF-8 titles). */
static int value_bytes_ok(const char *v)
{
	for (size_t i = 0; v[i] != '\0'; i++) {
		uint8_t b = (uint8_t)v[i];

		if ((b < 0x20 && b != '\t') || b == 0x7F) {
			return 0;
		}
	}
	return 1;
}

sicha_status sicha_header_validate(const char *name, const char *value)
{
	if (name == NULL || value == NULL || name[0] == '\0') {
		return SICHA_E_INVALID_ARG;
	}
	for (size_t i = 0; name[i] != '\0'; i++) {
		if (!is_token_char((uint8_t)name[i])) {
			return SICHA_E_INVALID_ARG;
		}
	}
	if (!value_bytes_ok(value)) {
		return SICHA_E_INVALID_ARG;
	}
	return SICHA_OK;
}

void sicha_headers_free(sicha_header *h, size_t n)
{
	if (h == NULL) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		/* casts drop the API-level const; we allocated these */
		free((char *)(uintptr_t)h[i].name);
		free((char *)(uintptr_t)h[i].value);
	}
	free(h);
}

sicha_status sicha_build_headers(const sicha_backend *b, int stream,
	const char *user_agent, sicha_header **out, size_t *n_out)
{
	size_t n = 3 + (b->api_key != NULL ? 1u : 0u) + b->n_extra_headers;
	sicha_header *h = calloc(n, sizeof(*h));
	size_t k = 0;

	*out = NULL;
	*n_out = 0;
	if (h == NULL) {
		return SICHA_E_NOMEM;
	}
	h[k].name = dup_cstr("Content-Type");
	h[k].value = dup_cstr("application/json");
	k++;
	h[k].name = dup_cstr("Accept");
	h[k].value = dup_cstr(stream ? "text/event-stream" :
		"application/json");
	k++;
	h[k].name = dup_cstr("User-Agent");
	h[k].value = dup_cstr(user_agent);
	k++;
	if (b->api_key != NULL) {
		size_t klen = strlen(b->api_key);
		char *v = malloc(klen + 8);

		h[k].name = dup_cstr("Authorization");
		if (v != NULL) {
			memcpy(v, "Bearer ", 7);
			memcpy(v + 7, b->api_key, klen + 1);
		}
		h[k].value = v;
		k++;
	}
	for (size_t i = 0; i < b->n_extra_headers; i++) {
		h[k].name = dup_cstr(b->extra_headers[i].name);
		h[k].value = dup_cstr(b->extra_headers[i].value);
		k++;
	}
	for (size_t i = 0; i < n; i++) {
		if (h[i].name == NULL || h[i].value == NULL) {
			sicha_headers_free(h, n);
			return SICHA_E_NOMEM;
		}
	}
	*out = h;
	*n_out = n;
	return SICHA_OK;
}

/* ------------------------------------------------------------------ */
/* Backend resolution                                                  */
/* ------------------------------------------------------------------ */

static void timeouts_resolve(sicha_timeouts *t)
{
	if (t->connect_ms == 0) {
		t->connect_ms = SICHA_DEFAULT_CONNECT_MS;
	}
	if (t->first_byte_ms == 0) {
		t->first_byte_ms = SICHA_DEFAULT_FIRST_BYTE_MS;
	}
	if (t->idle_ms == 0) {
		t->idle_ms = SICHA_DEFAULT_IDLE_MS;
	}
	if (t->total_ms == 0) {
		t->total_ms = SICHA_DEFAULT_TOTAL_MS;
	}
}

/* Header names sicha generates itself; extras must not collide. */
static int is_reserved_header(const char *name, int have_api_key)
{
	if (sicha_header_name_eq(name, "Content-Type") ||
		sicha_header_name_eq(name, "Accept") ||
		sicha_header_name_eq(name, "User-Agent") ||
		sicha_header_name_eq(name, "Content-Length") ||
		sicha_header_name_eq(name, "Host") ||
		sicha_header_name_eq(name, "Transfer-Encoding")) {
		return 1;
	}
	if (have_api_key &&
		sicha_header_name_eq(name, "Authorization")) {
		return 1;
	}
	return 0;
}

/* Must parse as a JSON object (the thing we can merge into a body). */
static sicha_status validate_json_object(const char *json)
{
	yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
	int ok;

	if (doc == NULL) {
		return SICHA_E_INVALID_ARG;
	}
	ok = yyjson_is_obj(yyjson_doc_get_root(doc));
	yyjson_doc_free(doc);
	return ok ? SICHA_OK : SICHA_E_INVALID_ARG;
}

sicha_status sicha_backend_init(sicha_backend *b,
	const sicha_backend_desc *desc)
{
	const uint32_t known_flags = SICHA_BACKEND_KOBOLD_CANCEL_ASSIST |
		SICHA_BACKEND_STREAM_USAGE;
	sicha_status st;

	memset(b, 0, sizeof(*b));
	if (desc->struct_size != (uint32_t)sizeof(*desc)) {
		return SICHA_E_INVALID_ARG;
	}
	if ((desc->flags & ~known_flags) != 0) {
		return SICHA_E_INVALID_ARG;
	}
	st = sicha_url_validate(desc->base_url);
	if (st != SICHA_OK) {
		return st;
	}
	if (desc->model == NULL || desc->model[0] == '\0' ||
		!sicha_utf8_valid(desc->model, strlen(desc->model))) {
		return SICHA_E_INVALID_ARG;
	}
	if (desc->api_key != NULL && (desc->api_key[0] == '\0' ||
		!value_bytes_ok(desc->api_key))) {
		return SICHA_E_INVALID_ARG;
	}
	if (desc->n_extra_headers > 0 && desc->extra_headers == NULL) {
		return SICHA_E_INVALID_ARG;
	}
	for (size_t i = 0; i < desc->n_extra_headers; i++) {
		const sicha_header *h = &desc->extra_headers[i];

		st = sicha_header_validate(h->name, h->value);
		if (st != SICHA_OK) {
			return st;
		}
		if (is_reserved_header(h->name, desc->api_key != NULL)) {
			return SICHA_E_INVALID_ARG;
		}
	}
	if (desc->extra_body_json != NULL) {
		st = validate_json_object(desc->extra_body_json);
		if (st != SICHA_OK) {
			return st;
		}
	}

	b->base_url = dup_cstr(desc->base_url);
	b->url_chat = b->base_url == NULL ? NULL :
		sicha_url_join(desc->base_url, "/chat/completions");
	b->model = dup_cstr(desc->model);
	b->api_key = desc->api_key != NULL ? dup_cstr(desc->api_key) :
		NULL;
	b->extra_body_json = desc->extra_body_json != NULL ?
		dup_cstr(desc->extra_body_json) : NULL;
	if (desc->flags & SICHA_BACKEND_KOBOLD_CANCEL_ASSIST) {
		char *origin = sicha_url_origin(desc->base_url);

		b->url_abort = origin == NULL ? NULL :
			sicha_url_join(origin, "/api/extra/abort");
		free(origin);
		if (b->url_abort == NULL) {
			sicha_backend_free(b);
			return SICHA_E_NOMEM;
		}
	}
	if (desc->n_extra_headers > 0) {
		b->extra_headers = calloc(desc->n_extra_headers,
			sizeof(*b->extra_headers));
		if (b->extra_headers == NULL) {
			sicha_backend_free(b);
			return SICHA_E_NOMEM;
		}
		b->n_extra_headers = desc->n_extra_headers;
		for (size_t i = 0; i < desc->n_extra_headers; i++) {
			b->extra_headers[i].name =
				dup_cstr(desc->extra_headers[i].name);
			b->extra_headers[i].value =
				dup_cstr(desc->extra_headers[i].value);
			if (b->extra_headers[i].name == NULL ||
				b->extra_headers[i].value == NULL) {
				sicha_backend_free(b);
				return SICHA_E_NOMEM;
			}
		}
	}
	if (b->base_url == NULL || b->url_chat == NULL ||
		b->model == NULL ||
		(desc->api_key != NULL && b->api_key == NULL) ||
		(desc->extra_body_json != NULL &&
			b->extra_body_json == NULL)) {
		sicha_backend_free(b);
		return SICHA_E_NOMEM;
	}
	b->timeouts = desc->timeouts;
	timeouts_resolve(&b->timeouts);
	b->flags = desc->flags;
	return SICHA_OK;
}

void sicha_backend_free(sicha_backend *b)
{
	free(b->base_url);
	free(b->url_chat);
	free(b->url_abort);
	free(b->api_key);
	free(b->model);
	if (b->extra_headers != NULL) {
		for (size_t i = 0; i < b->n_extra_headers; i++) {
			free((char *)(uintptr_t)b->extra_headers[i].name);
			free((char *)(uintptr_t)b->extra_headers[i].value);
		}
		free(b->extra_headers);
	}
	free(b->extra_body_json);
	memset(b, 0, sizeof(*b));
}

/* ------------------------------------------------------------------ */
/* Chat body serialization                                             */
/* ------------------------------------------------------------------ */

static const char *role_str(int32_t role)
{
	switch (role) {
	case SICHA_ROLE_SYSTEM:
		return "system";
	case SICHA_ROLE_USER:
		return "user";
	case SICHA_ROLE_ASSISTANT:
		return "assistant";
	case SICHA_ROLE_TOOL:
		return "tool";
	default:
		return NULL;
	}
}

/* Also used by the engine to reject a request up front, before any
 * result exists.  Complete: after this passes, body building can only
 * fail on OOM. */
sicha_status sicha_request_validate(const sicha_request *req)
{
	const uint32_t known_set = SICHA_SET_MAX_TOKENS |
		SICHA_SET_TEMPERATURE | SICHA_SET_TOP_P | SICHA_SET_TOP_K |
		SICHA_SET_PRESENCE_PENALTY | SICHA_SET_FREQUENCY_PENALTY;
	const uint32_t known_flags = SICHA_REQ_NO_ACCUMULATE;
	int have_array = req->messages != NULL || req->n_messages > 0;

	if (req->struct_size != (uint32_t)sizeof(*req)) {
		return SICHA_E_INVALID_ARG;
	}
	if ((req->set_mask & ~known_set) != 0 ||
		(req->flags & ~known_flags) != 0) {
		return SICHA_E_INVALID_ARG;
	}
	if (have_array == (req->messages_json != NULL)) {
		return SICHA_E_INVALID_ARG; /* exactly one source */
	}
	if (have_array && (req->messages == NULL || req->n_messages == 0)) {
		return SICHA_E_INVALID_ARG;
	}
	for (size_t i = 0; i < req->n_messages && have_array; i++) {
		const sicha_message *m = &req->messages[i];
		size_t clen;

		if (role_str(m->role) == NULL || m->content == NULL) {
			return SICHA_E_INVALID_ARG;
		}
		clen = m->content_len == SICHA_LEN_CSTR ?
			strlen(m->content) : m->content_len;
		if (!sicha_utf8_valid(m->content, clen)) {
			return SICHA_E_INVALID_ARG;
		}
		if (m->tool_call_id != NULL) {
			if (m->role != SICHA_ROLE_TOOL ||
				m->tool_call_id[0] == '\0' ||
				!sicha_utf8_valid(m->tool_call_id,
					strlen(m->tool_call_id))) {
				return SICHA_E_INVALID_ARG;
			}
		}
	}
	if ((req->set_mask & SICHA_SET_MAX_TOKENS) && req->max_tokens < 1) {
		return SICHA_E_INVALID_ARG;
	}
	if ((req->set_mask & SICHA_SET_TOP_K) && req->top_k < 0) {
		return SICHA_E_INVALID_ARG;
	}
	if (((req->set_mask & SICHA_SET_TEMPERATURE) &&
			!(isfinite(req->temperature) &&
				req->temperature >= 0.0)) ||
		((req->set_mask & SICHA_SET_TOP_P) &&
			!isfinite(req->top_p)) ||
		((req->set_mask & SICHA_SET_PRESENCE_PENALTY) &&
			!isfinite(req->presence_penalty)) ||
		((req->set_mask & SICHA_SET_FREQUENCY_PENALTY) &&
			!isfinite(req->frequency_penalty))) {
		return SICHA_E_INVALID_ARG;
	}
	if (req->n_stop > 0 && req->stop == NULL) {
		return SICHA_E_INVALID_ARG;
	}
	for (size_t i = 0; i < req->n_stop; i++) {
		if (req->stop[i] == NULL || req->stop[i][0] == '\0' ||
			!sicha_utf8_valid(req->stop[i],
				strlen(req->stop[i]))) {
			return SICHA_E_INVALID_ARG;
		}
	}
	if (req->messages_json != NULL) {
		yyjson_doc *doc = yyjson_read(req->messages_json,
			strlen(req->messages_json), 0);
		int ok = doc != NULL &&
			yyjson_is_arr(yyjson_doc_get_root(doc));

		yyjson_doc_free(doc);
		if (!ok) {
			return SICHA_E_INVALID_ARG;
		}
	}
	if (req->response_format_json != NULL) {
		sicha_status st =
			validate_json_object(req->response_format_json);

		if (st != SICHA_OK) {
			return st;
		}
	}
	return SICHA_OK;
}

/* Parse `json`, copy its root into `doc`, require `expect_obj` /
 * array accordingly.  NULL on parse failure, wrong type, or OOM
 * (*oom set on OOM so the caller can distinguish). */
static yyjson_mut_val *import_json(yyjson_mut_doc *doc, const char *json,
	int expect_obj, int *oom)
{
	yyjson_doc *src = yyjson_read(json, strlen(json), 0);
	yyjson_val *root;
	yyjson_mut_val *copy;

	if (src == NULL) {
		return NULL;
	}
	root = yyjson_doc_get_root(src);
	if (expect_obj ? !yyjson_is_obj(root) : !yyjson_is_arr(root)) {
		yyjson_doc_free(src);
		return NULL;
	}
	copy = yyjson_val_mut_copy(doc, root);
	if (copy == NULL) {
		*oom = 1;
	}
	yyjson_doc_free(src);
	return copy;
}

sicha_status sicha_build_chat_body(const sicha_backend *b,
	const sicha_request *req, int stream, char **out, size_t *out_len)
{
	yyjson_mut_doc *doc;
	yyjson_mut_val *root;
	sicha_status st;
	int oom = 0;

	*out = NULL;
	*out_len = 0;
	st = sicha_request_validate(req);
	if (st != SICHA_OK) {
		return st;
	}
	doc = yyjson_mut_doc_new(NULL);
	if (doc == NULL) {
		return SICHA_E_NOMEM;
	}
	root = yyjson_mut_obj(doc);
	if (root == NULL) {
		goto oom;
	}
	yyjson_mut_doc_set_root(doc, root);

	if (!yyjson_mut_obj_add_str(doc, root, "model", b->model)) {
		goto oom;
	}

	if (req->messages_json != NULL) {
		yyjson_mut_val *arr = import_json(doc, req->messages_json,
			0, &oom);

		if (arr == NULL) {
			st = oom ? SICHA_E_NOMEM : SICHA_E_INVALID_ARG;
			goto fail;
		}
		if (!yyjson_mut_obj_add_val(doc, root, "messages", arr)) {
			goto oom;
		}
	} else {
		yyjson_mut_val *arr = yyjson_mut_arr(doc);

		if (arr == NULL ||
			!yyjson_mut_obj_add_val(doc, root, "messages", arr)) {
			goto oom;
		}
		for (size_t i = 0; i < req->n_messages; i++) {
			const sicha_message *m = &req->messages[i];
			size_t clen = m->content_len == SICHA_LEN_CSTR ?
				strlen(m->content) : m->content_len;
			yyjson_mut_val *obj = yyjson_mut_obj(doc);

			if (obj == NULL ||
				!yyjson_mut_arr_add_val(arr, obj)) {
				goto oom;
			}
			if (!yyjson_mut_obj_add_str(doc, obj, "role",
					role_str(m->role)) ||
				!yyjson_mut_obj_add_strn(doc, obj, "content",
					m->content, clen)) {
				goto oom;
			}
			if (m->tool_call_id != NULL &&
				!yyjson_mut_obj_add_str(doc, obj,
					"tool_call_id", m->tool_call_id)) {
				goto oom;
			}
		}
	}

	if (stream) {
		if (!yyjson_mut_obj_add_bool(doc, root, "stream", true)) {
			goto oom;
		}
		if (b->flags & SICHA_BACKEND_STREAM_USAGE) {
			yyjson_mut_val *so = yyjson_mut_obj(doc);

			if (so == NULL ||
				!yyjson_mut_obj_add_bool(doc, so,
					"include_usage", true) ||
				!yyjson_mut_obj_add_val(doc, root,
					"stream_options", so)) {
				goto oom;
			}
		}
	}

	if ((req->set_mask & SICHA_SET_MAX_TOKENS) &&
		!yyjson_mut_obj_add_int(doc, root, "max_tokens",
			req->max_tokens)) {
		goto oom;
	}
	if ((req->set_mask & SICHA_SET_TEMPERATURE) &&
		!yyjson_mut_obj_add_real(doc, root, "temperature",
			req->temperature)) {
		goto oom;
	}
	if ((req->set_mask & SICHA_SET_TOP_P) &&
		!yyjson_mut_obj_add_real(doc, root, "top_p", req->top_p)) {
		goto oom;
	}
	if ((req->set_mask & SICHA_SET_TOP_K) &&
		!yyjson_mut_obj_add_int(doc, root, "top_k", req->top_k)) {
		goto oom;
	}
	if ((req->set_mask & SICHA_SET_PRESENCE_PENALTY) &&
		!yyjson_mut_obj_add_real(doc, root, "presence_penalty",
			req->presence_penalty)) {
		goto oom;
	}
	if ((req->set_mask & SICHA_SET_FREQUENCY_PENALTY) &&
		!yyjson_mut_obj_add_real(doc, root, "frequency_penalty",
			req->frequency_penalty)) {
		goto oom;
	}

	if (req->n_stop > 0) {
		yyjson_mut_val *arr = yyjson_mut_arr(doc);

		if (arr == NULL ||
			!yyjson_mut_obj_add_val(doc, root, "stop", arr)) {
			goto oom;
		}
		for (size_t i = 0; i < req->n_stop; i++) {
			if (!yyjson_mut_arr_add_str(doc, arr,
					req->stop[i])) {
				goto oom;
			}
		}
	}

	if (req->response_format_json != NULL) {
		yyjson_mut_val *rf = import_json(doc,
			req->response_format_json, 1, &oom);

		if (rf == NULL) {
			st = oom ? SICHA_E_NOMEM : SICHA_E_INVALID_ARG;
			goto fail;
		}
		if (!yyjson_mut_obj_add_val(doc, root, "response_format",
				rf)) {
			goto oom;
		}
	}

	/* extra body merges last: on key collision the extra wins.  The
	 * extra doc is read immutably and every pair is copied fresh —
	 * a yyjson_mut_val may only ever belong to one container. */
	if (b->extra_body_json != NULL) {
		yyjson_doc *src = yyjson_read(b->extra_body_json,
			strlen(b->extra_body_json), 0);
		yyjson_obj_iter it;
		yyjson_val *key;

		if (src == NULL) {
			/* validated at backend init: only OOM here */
			goto oom;
		}
		yyjson_obj_iter_init(yyjson_doc_get_root(src), &it);
		while ((key = yyjson_obj_iter_next(&it)) != NULL) {
			const char *ks = yyjson_get_str(key);
			size_t kl = yyjson_get_len(key);
			yyjson_mut_val *k2 = yyjson_mut_strncpy(doc, ks, kl);
			yyjson_mut_val *v2 = yyjson_val_mut_copy(doc,
				yyjson_obj_iter_get_val(key));

			if (k2 == NULL || v2 == NULL) {
				yyjson_doc_free(src);
				goto oom;
			}
			yyjson_mut_obj_remove_keyn(root, ks, kl);
			if (!yyjson_mut_obj_add(root, k2, v2)) {
				yyjson_doc_free(src);
				goto oom;
			}
		}
		yyjson_doc_free(src);
	}

	{
		size_t n = 0;
		char *json = yyjson_mut_write(doc, 0, &n);

		if (json == NULL) {
			goto oom;
		}
		*out = json;
		*out_len = n;
	}
	yyjson_mut_doc_free(doc);
	return SICHA_OK;

oom:
	st = SICHA_E_NOMEM;
fail:
	yyjson_mut_doc_free(doc);
	return st;
}
