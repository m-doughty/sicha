/* libFuzzer harness for backend resolution, header assembly, and
 * chat-body serialization (white-box).  Fuzz bytes script a backend
 * desc and a request full of hostile strings (invalid UTF-8, huge
 * masks, NaN-pattern doubles, junk JSON).  Oracles:
 *   - no crash / sanitizer report on any input;
 *   - build either rejects (INVALID_ARG) or emits VALID JSON: parses
 *     with yyjson, root is an object, model and messages round-trip
 *     byte-exactly (array-source requests), output is valid UTF-8;
 *   - building twice yields identical bytes (no nondeterminism, no
 *     uninitialized reads steering output);
 *   - assembled headers never contain CR / LF and Authorization is
 *     exactly "Bearer <key>".
 *
 * Build with -DSICHA_FUZZ=ON (Clang only); run e.g.
 *   ./fuzz_request -max_total_time=60 corpus/
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sicha_internal.h"
#include "yyjson.h"

#define FUZZ_ASSERT(cond) \
	do { \
		if (!(cond)) { \
			abort(); \
		} \
	} while (0)

typedef struct rd {
	const uint8_t *d;
	size_t n;
	size_t i;
} rd;

static uint8_t r8(rd *r)
{
	return r->i < r->n ? r->d[r->i++] : 0;
}

static uint32_t r32(rd *r)
{
	uint32_t v = 0;

	for (int k = 0; k < 4; k++) {
		v = (v << 8) | r8(r);
	}
	return v;
}

static double rdouble(rd *r)
{
	uint8_t mode = r8(r);

	if (mode < 180) {
		return (double)(mode % 40) / 10.0 - 1.0;
	}
	{
		uint64_t bits = ((uint64_t)r32(r) << 32) | r32(r);
		double f;

		memcpy(&f, &bits, sizeof(f)); /* NaN / inf territory */
		return f;
	}
}

/* NUL-terminated slice of fuzz bytes into `scratch`. */
static const char *rstr(rd *r, char *scratch, size_t scratch_cap)
{
	size_t want = r8(r) % scratch_cap;
	size_t have = r->n - r->i < want ? r->n - r->i : want;

	memcpy(scratch, r->d + r->i, have);
	scratch[have] = '\0';
	r->i += have;
	return scratch;
}

static const char *maybe(rd *r, const char *s)
{
	return r8(r) % 3 == 0 ? NULL : s;
}

static void check_headers(const sicha_backend *b, const char *api_key)
{
	sicha_header *h = NULL;
	size_t n = 0;
	sicha_status st = sicha_build_headers(b, 1, "sicha-fuzz/1", &h,
		&n);

	if (st != SICHA_OK) {
		FUZZ_ASSERT(st == SICHA_E_NOMEM);
		return;
	}
	for (size_t i = 0; i < n; i++) {
		FUZZ_ASSERT(h[i].name != NULL && h[i].value != NULL);
		FUZZ_ASSERT(strchr(h[i].value, '\r') == NULL);
		FUZZ_ASSERT(strchr(h[i].value, '\n') == NULL);
		FUZZ_ASSERT(strchr(h[i].name, '\r') == NULL);
		FUZZ_ASSERT(strchr(h[i].name, '\n') == NULL);
		if (sicha_header_name_eq(h[i].name, "Authorization")) {
			FUZZ_ASSERT(api_key != NULL);
			FUZZ_ASSERT(strncmp(h[i].value, "Bearer ", 7) == 0);
			FUZZ_ASSERT(strcmp(h[i].value + 7, api_key) == 0);
		}
	}
	sicha_headers_free(h, n);
}

static void check_body_json(const char *json, size_t len,
	const sicha_backend *b, const sicha_request *req)
{
	yyjson_doc *doc = yyjson_read(json, len, 0);
	yyjson_val *root;
	yyjson_val *msgs;

	FUZZ_ASSERT(doc != NULL);
	root = yyjson_doc_get_root(doc);
	FUZZ_ASSERT(yyjson_is_obj(root));
	FUZZ_ASSERT(sicha_utf8_valid(json, len));

	msgs = yyjson_obj_get(root, "messages");
	FUZZ_ASSERT(msgs != NULL && yyjson_is_arr(msgs));
	if (b->extra_body_json == NULL) {
		yyjson_val *model = yyjson_obj_get(root, "model");

		FUZZ_ASSERT(model != NULL && yyjson_is_str(model));
		FUZZ_ASSERT(strcmp(yyjson_get_str(model), b->model) == 0);
	}
	if (req->messages != NULL && b->extra_body_json == NULL) {
		size_t i = 0;
		yyjson_val *m;
		yyjson_arr_iter it;

		FUZZ_ASSERT(yyjson_arr_size(msgs) == req->n_messages);
		yyjson_arr_iter_init(msgs, &it);
		while ((m = yyjson_arr_iter_next(&it)) != NULL) {
			yyjson_val *content = yyjson_obj_get(m, "content");
			size_t clen = req->messages[i].content_len ==
				SICHA_LEN_CSTR ?
				strlen(req->messages[i].content) :
				req->messages[i].content_len;

			FUZZ_ASSERT(yyjson_is_str(content));
			FUZZ_ASSERT(yyjson_get_len(content) == clen);
			FUZZ_ASSERT(memcmp(yyjson_get_str(content),
				req->messages[i].content, clen) == 0);
			i++;
		}
	}
	yyjson_doc_free(doc);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	rd r = { data, size, 0 };
	char s_url[64];
	char s_model[48];
	char s_key[48];
	char s_extra[96];
	char s_hname[24];
	char s_hval[48];
	char s_content[4][96];
	char s_msgs_json[128];
	char s_rf[96];
	char s_stop[2][24];
	sicha_backend_desc desc;
	sicha_request req;
	sicha_message msgs[4];
	const char *stops[2];
	sicha_header extra_h;
	sicha_backend b;
	sicha_status st;

	memset(&desc, 0, sizeof(desc));
	desc.struct_size = r8(&r) % 16 == 0 ? r32(&r) :
		(uint32_t)sizeof(desc);
	desc.base_url = r8(&r) % 2 == 0 ? "http://fuzz.local:5001/v1" :
		rstr(&r, s_url, sizeof(s_url));
	desc.model = rstr(&r, s_model, sizeof(s_model));
	desc.api_key = maybe(&r, rstr(&r, s_key, sizeof(s_key)));
	desc.extra_body_json = maybe(&r, rstr(&r, s_extra,
		sizeof(s_extra)));
	desc.flags = r8(&r) % 4 == 0 ? r32(&r) : (r8(&r) % 4);
	desc.timeouts.connect_ms = r8(&r) % 8 == 0 ? r32(&r) : 0;
	if (r8(&r) % 3 == 0) {
		extra_h.name = rstr(&r, s_hname, sizeof(s_hname));
		extra_h.value = rstr(&r, s_hval, sizeof(s_hval));
		desc.extra_headers = &extra_h;
		desc.n_extra_headers = 1;
	}

	memset(&req, 0, sizeof(req));
	req.struct_size = r8(&r) % 16 == 0 ? r32(&r) :
		(uint32_t)sizeof(req);
	if (r8(&r) % 5 == 0) {
		req.messages_json = rstr(&r, s_msgs_json,
			sizeof(s_msgs_json));
	} else {
		size_t n = 1 + r8(&r) % 4;

		for (size_t i = 0; i < n; i++) {
			msgs[i].role = r8(&r) % 8 == 0 ? (int32_t)r32(&r) :
				(int32_t)(r8(&r) % 4);
			msgs[i].content = rstr(&r, s_content[i],
				sizeof(s_content[i]));
			msgs[i].content_len = r8(&r) % 2 == 0 ?
				SICHA_LEN_CSTR : strlen(msgs[i].content);
			msgs[i].tool_call_id = r8(&r) % 8 == 0 ?
				"call_1" : NULL;
		}
		req.messages = msgs;
		req.n_messages = r8(&r) % 16 == 0 ? 0 : n;
	}
	req.set_mask = r8(&r) % 4 == 0 ? r32(&r) : (r8(&r) % 64);
	req.max_tokens = (int32_t)r32(&r) % 4096;
	req.temperature = rdouble(&r);
	req.top_p = rdouble(&r);
	req.top_k = (int32_t)r32(&r) % 512;
	req.presence_penalty = rdouble(&r);
	req.frequency_penalty = rdouble(&r);
	if (r8(&r) % 4 == 0) {
		stops[0] = rstr(&r, s_stop[0], sizeof(s_stop[0]));
		stops[1] = rstr(&r, s_stop[1], sizeof(s_stop[1]));
		req.stop = stops;
		req.n_stop = 1 + r8(&r) % 2;
	}
	req.response_format_json = maybe(&r, rstr(&r, s_rf, sizeof(s_rf)));
	req.flags = r8(&r) % 8 == 0 ? r32(&r) : 0;

	st = sicha_backend_init(&b, &desc);
	FUZZ_ASSERT(st == SICHA_OK || st == SICHA_E_INVALID_ARG ||
		st == SICHA_E_NOMEM);
	if (st != SICHA_OK) {
		return 0;
	}
	check_headers(&b, desc.api_key);

	{
		int stream = r8(&r) % 2;
		char *out1 = NULL;
		char *out2 = NULL;
		size_t len1 = 0;
		size_t len2 = 0;
		sicha_status st1 = sicha_build_chat_body(&b, &req, stream,
			&out1, &len1);
		sicha_status st2 = sicha_build_chat_body(&b, &req, stream,
			&out2, &len2);

		FUZZ_ASSERT(st1 == SICHA_OK || st1 == SICHA_E_INVALID_ARG ||
			st1 == SICHA_E_NOMEM);
		FUZZ_ASSERT(st1 == st2);
		if (st1 == SICHA_OK) {
			FUZZ_ASSERT(out1 != NULL && out2 != NULL);
			FUZZ_ASSERT(len1 == len2);
			FUZZ_ASSERT(memcmp(out1, out2, len1) == 0);
			FUZZ_ASSERT(out1[len1] == '\0');
			check_body_json(out1, len1, &b, &req);
		} else {
			FUZZ_ASSERT(out1 == NULL && out2 == NULL);
		}
		free(out1);
		free(out2);
	}
	sicha_backend_free(&b);
	return 0;
}
