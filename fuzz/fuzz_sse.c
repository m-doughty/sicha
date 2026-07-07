/* libFuzzer harness for the SSE parser (white-box: includes the
 * internal header).  The first bytes choose a chunking strategy; the
 * rest is the stream.  Oracles:
 *   - no crash / no sanitizer report on any input;
 *   - partition invariance: parsing the payload byte-split per the
 *     strategy yields the identical event sequence (count, total
 *     bytes, FNV-1a digest) and the identical terminal status as
 *     parsing it in one shot;
 *   - the event-size cap terminates parsing with the same result
 *     regardless of partitioning.
 *
 * Build with -DSICHA_FUZZ=ON (Clang only); run e.g.
 *   ./fuzz_sse -max_total_time=60 corpus/
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sicha_internal.h"

#define FUZZ_ASSERT(cond) \
	do { \
		if (!(cond)) { \
			abort(); \
		} \
	} while (0)

typedef struct digest {
	uint64_t fnv;
	uint64_t total_bytes;
	uint32_t events;
} digest;

static int32_t digest_event(void *ud, const char *data, size_t len)
{
	digest *d = ud;

	for (size_t i = 0; i < len; i++) {
		d->fnv ^= (uint8_t)data[i];
		d->fnv *= 0x100000001B3ull;
	}
	/* separate events so "ab"+"c" != "a"+"bc" */
	d->fnv ^= 0xFF;
	d->fnv *= 0x100000001B3ull;
	d->total_bytes += len;
	d->events++;
	return 0;
}

/* Parse with a chunk-size schedule; returns the final status and the
 * combined feed/finish return code. */
static sicha_status run(const char *payload, size_t len,
	size_t max_event, const uint8_t *schedule, size_t schedule_len,
	digest *out, int32_t *rc_out)
{
	sicha_sse p;
	size_t pos = 0;
	size_t s = 0;
	int32_t rc = 0;
	sicha_status st;

	sicha_sse_init(&p, max_event);
	while (pos < len) {
		size_t n = len - pos;

		if (schedule_len > 0) {
			size_t want = (size_t)schedule[s % schedule_len] + 1;

			s++;
			if (want < n) {
				n = want;
			}
		}
		rc = sicha_sse_feed(&p, payload + pos, n, digest_event,
			out);
		if (rc != 0) {
			break;
		}
		pos += n;
	}
	if (rc == 0) {
		rc = sicha_sse_finish(&p, digest_event, out);
	}
	st = sicha_sse_status(&p);
	sicha_sse_free(&p);
	*rc_out = rc;
	return st;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	uint8_t schedule[8];
	size_t schedule_len;
	size_t max_event;
	const char *payload;
	size_t payload_len;
	digest whole = { 0xcbf29ce484222325ull, 0, 0 };
	digest split = { 0xcbf29ce484222325ull, 0, 0 };
	int32_t rc_whole;
	int32_t rc_split;
	sicha_status st_whole;
	sicha_status st_split;

	if (size < 10) {
		return 0;
	}
	/* header: 1 byte cap selector, 1 byte schedule length, then the
	 * schedule bytes */
	max_event = data[0] % 4 == 0 ? 1u + (size_t)data[0] * 16u : 0;
	schedule_len = 1 + (data[1] % sizeof(schedule));
	if (size < 2 + schedule_len) {
		return 0;
	}
	memcpy(schedule, data + 2, schedule_len);
	payload = (const char *)data + 2 + schedule_len;
	payload_len = size - 2 - schedule_len;

	st_whole = run(payload, payload_len, max_event, NULL, 0, &whole,
		&rc_whole);
	st_split = run(payload, payload_len, max_event, schedule,
		schedule_len, &split, &rc_split);

	FUZZ_ASSERT(st_whole == st_split);
	FUZZ_ASSERT(rc_whole == rc_split);
	FUZZ_ASSERT(whole.fnv == split.fnv);
	FUZZ_ASSERT(whole.total_bytes == split.total_bytes);
	FUZZ_ASSERT(whole.events == split.events);
	if (max_event > 0) {
		/* no single event may exceed the cap: the digest walk
		 * cannot verify per-event size (already folded), but the
		 * status must reflect a violation */
		FUZZ_ASSERT(st_whole == SICHA_OK ||
			st_whole == SICHA_INT_E_TOOBIG ||
			st_whole == SICHA_E_NOMEM);
	} else {
		FUZZ_ASSERT(st_whole == SICHA_OK ||
			st_whole == SICHA_E_NOMEM);
	}
	return 0;
}
