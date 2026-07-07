/* Incremental Server-Sent-Events parser.
 *
 * Wire reality this must survive (observed against OpenRouter,
 * KoboldCpp, and assorted OAI-compatible gateways):
 *   - events split at arbitrary byte positions across TCP reads;
 *   - CRLF, LF, and bare CR line terminators, including a CR/LF pair
 *     split across two reads;
 *   - comment heartbeats (": OPENROUTER PROCESSING");
 *   - a UTF-8 BOM before the first event, sometimes split too;
 *   - streams that end without the final dispatching blank line
 *     (handled leniently by sicha_sse_finish).
 *
 * The parser is generic SSE: it knows nothing about JSON or
 * "[DONE]" — higher layers interpret event payloads. */

#include <string.h>

#include "sicha_internal.h"

void sicha_sse_init(sicha_sse *p, size_t max_event_bytes)
{
	sicha_buf_init(&p->line, max_event_bytes);
	sicha_buf_init(&p->data, max_event_bytes);
	p->have_data = 0;
	p->last_was_cr = 0;
	p->bom_seen = 0;
	p->bom_decided = 0;
	p->max_event_bytes = max_event_bytes;
	p->err = SICHA_OK;
}

void sicha_sse_free(sicha_sse *p)
{
	sicha_buf_free(&p->line);
	sicha_buf_free(&p->data);
}

sicha_status sicha_sse_status(const sicha_sse *p)
{
	return p->err;
}

/* Sticky-error promotion from the internal buffers. */
static int sse_check_bufs(sicha_sse *p)
{
	if (p->err != SICHA_OK) {
		return 0;
	}
	if (sicha_buf_status(&p->line) != SICHA_OK) {
		p->err = sicha_buf_status(&p->line);
		return 0;
	}
	if (sicha_buf_status(&p->data) != SICHA_OK) {
		p->err = sicha_buf_status(&p->data);
		return 0;
	}
	return 1;
}

/* A complete line (terminator stripped) arrived. */
static int32_t sse_line(sicha_sse *p, const char *line, size_t len,
	sicha_sse_on_event on_event, void *ud)
{
	if (len == 0) {
		/* blank line: dispatch the pending event, if any */
		int32_t rc = 0;

		if (p->have_data) {
			if (on_event != NULL) {
				rc = on_event(ud, p->data.data == NULL ? "" :
					p->data.data, p->data.len);
			}
			sicha_buf_reset(&p->data);
			p->have_data = 0;
		}
		return rc;
	}
	if (line[0] == ':') {
		return 0; /* comment / heartbeat */
	}
	/* field: "name" or "name: value" (one leading space stripped) */
	{
		size_t name_len = len;
		const char *value = "";
		size_t value_len = 0;
		const char *colon = memchr(line, ':', len);

		if (colon != NULL) {
			name_len = (size_t)(colon - line);
			value = colon + 1;
			value_len = len - name_len - 1;
			if (value_len > 0 && value[0] == ' ') {
				value++;
				value_len--;
			}
		}
		if (name_len == 4 && memcmp(line, "data", 4) == 0) {
			if (p->have_data) {
				sicha_buf_append_ch(&p->data, '\n');
			}
			sicha_buf_append(&p->data, value, value_len);
			p->have_data = 1;
			if (!sse_check_bufs(p)) {
				return -1;
			}
		}
		/* event: / id: / retry: / anything else — ignored */
	}
	return 0;
}

int32_t sicha_sse_feed(sicha_sse *p, const char *bytes, size_t len,
	sicha_sse_on_event on_event, void *ud)
{
	static const char bom[3] = { '\xEF', '\xBB', '\xBF' };
	size_t i = 0;

	if (p->err != SICHA_OK) {
		return -1;
	}
	/* BOM stripping, robust to the BOM arriving split across feeds.
	 * Matched bytes are held back; on a mismatch they are replayed
	 * into the line buffer (they were real payload after all). */
	while (!p->bom_decided && i < len) {
		if (bytes[i] == bom[p->bom_seen]) {
			p->bom_seen++;
			i++;
			if (p->bom_seen == 3) {
				p->bom_decided = 1;
			}
			continue;
		}
		sicha_buf_append(&p->line, bom, p->bom_seen);
		p->bom_seen = 0;
		p->bom_decided = 1;
		if (!sse_check_bufs(p)) {
			return -1;
		}
	}
	for (; i < len; i++) {
		char ch = bytes[i];

		if (p->last_was_cr) {
			p->last_was_cr = 0;
			if (ch == '\n') {
				continue; /* second half of CRLF */
			}
		}
		if (ch == '\r' || ch == '\n') {
			int32_t rc = sse_line(p, p->line.data == NULL ? "" :
				p->line.data, p->line.len, on_event, ud);

			sicha_buf_reset(&p->line);
			p->last_was_cr = ch == '\r';
			if (rc != 0) {
				return rc;
			}
			if (p->err != SICHA_OK) {
				return -1;
			}
			continue;
		}
		sicha_buf_append_ch(&p->line, ch);
		if (!sse_check_bufs(p)) {
			return -1;
		}
	}
	return 0;
}

int32_t sicha_sse_finish(sicha_sse *p, sicha_sse_on_event on_event,
	void *ud)
{
	int32_t rc;

	if (p->err != SICHA_OK) {
		return -1;
	}
	/* Held-back BOM prefix that never completed is payload. */
	if (!p->bom_decided && p->bom_seen > 0) {
		static const char bom[3] = { '\xEF', '\xBB', '\xBF' };

		sicha_buf_append(&p->line, bom, p->bom_seen);
		p->bom_seen = 0;
	}
	p->bom_decided = 1;
	/* An unterminated final line still counts as a line... */
	if (p->line.len > 0) {
		rc = sse_line(p, p->line.data, p->line.len, on_event, ud);
		sicha_buf_reset(&p->line);
		if (rc != 0) {
			return rc;
		}
		if (p->err != SICHA_OK) {
			return -1;
		}
	}
	/* ...and a complete pending event is dispatched even without the
	 * final blank line (lenient EOF: some gateways end this way). */
	rc = sse_line(p, "", 0, on_event, ud);
	if (rc != 0) {
		return rc;
	}
	return p->err == SICHA_OK ? 0 : -1;
}
