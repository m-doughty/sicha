# sicha

**sicha** (שיחה, "conversation") is an LLM chat-completion client in
C17 — streaming and non-streaming — with a retry engine and ordered
multi-backend fallback routing built in.  It speaks to any
OpenAI-compatible `POST {base}/chat/completions` endpoint: local
engines (KoboldCpp, llama.cpp, vLLM, ...), aggregators (OpenRouter),
and hosted OAI-compatible APIs.

The complete contract lives in [`include/sicha.h`](include/sicha.h)
(spec-in-header).  Design highlights:

- **FFI-first API.**  Opaque handles, fixed-layout option structs with
  `struct_size`, error codes, no globals, no thread-local state.  All
  inputs are copied during the call; all outputs are NUL-terminated
  *and* length-carrying, owned by the result object.
- **Pluggable transport.**  The core (retry engine, SSE parser,
  request builder, response assembly) has zero system dependencies.
  A libcurl transport ships by default (`SICHA_WITH_CURL`, ON), and a
  fully **scripted in-memory transport is part of the public API** —
  your tests can exercise every wire condition (chunk boundaries,
  timeouts, resets, rate limits) deterministically and offline.
- **A ported, battle-tested retry design.**  Round trips classify into
  *abort* (auth/config: 400/401/402/403/404), *retry-same* (transient:
  connect failures, 5xx — exponential backoff with jitter, Retry-After
  honored), and *advance* (model pathology: client timeouts, 429,
  malformed bodies — fall through to the next backend immediately).
  Classification is overridable per HTTP status.  Exhaustion returns a
  full per-attempt record array (class, HTTP status, latency, body
  excerpt).  Cancellation is instant (condvar-backed tokens), mass
  cancel is one trigger, and cancelled KoboldCpp backends get a
  fire-and-forget `/api/extra/abort` assist.
- **Streaming that survives the real world.**  Events split across
  arbitrary TCP reads, CRLF/LF/CR, BOMs, comment heartbeats,
  interleaved error fragments, missing `[DONE]` terminators.  Deltas
  are delivered zero-copy to your callback.  Once your callback has
  seen bytes, a dying stream **never silently retries**
  (`SICHA_E_STREAM_LOST`) unless you opt in.
- **HTTP/1.1 forced** — several OAI-compatible gateways hang on
  HTTP/2 header frames.  First-byte timeouts can be set to
  `SICHA_INFINITE` for local engines that spend minutes in prefill.
- **Memory safety as a feature.**  ASan+UBSan and TSan CI lanes, four
  libFuzzer harnesses with committed corpora and a nightly fuzzing
  campaign, size caps on every response path, header-injection
  rejection at the boundary, and API keys that never appear in any
  log (`SICHA_DEBUG` wire logging redacts them).
- **Deterministic when you need it.**  Injectable clock and PRNG seed:
  retry schedules reproduce exactly in tests (no real sleeping), and a
  bitwise counter gate (`sicha_bench --check`) holds across three OSes
  in CI.

## Build

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `SICHA_WITH_CURL` (ON — the built-in transport; requires
libcurl headers), `SICHA_BUILD_TESTS` (ON), `SICHA_BUILD_BENCH`,
`SICHA_WERROR`, `SICHA_SANITIZE` (ASan+UBSan), `SICHA_TSAN`,
`SICHA_FUZZ` (Clang).  With `SICHA_WITH_CURL=OFF` the library builds
with no system dependencies at all (bring your own transport).

## Quickstart

```c
#include <stdio.h>
#include <sicha.h>

static int32_t on_delta(void *ud, const char *bytes, size_t len)
{
	(void)ud;
	fwrite(bytes, 1, len, stdout);
	fflush(stdout);
	return 0;
}

int main(void)
{
	/* an ordered fallback chain: local engine first, OpenRouter
	 * as the fallback */
	sicha_backend_desc backends[2] = { 0 };

	backends[0].struct_size = sizeof(backends[0]);
	backends[0].base_url = "http://localhost:5001/v1";
	backends[0].model = "local-model";
	backends[0].flags = SICHA_BACKEND_KOBOLD_CANCEL_ASSIST;
	backends[0].timeouts.first_byte_ms = SICHA_INFINITE; /* prefill */
	backends[0].extra_body_json = "{\"min_p\":0.05}";

	backends[1].struct_size = sizeof(backends[1]);
	backends[1].base_url = "https://openrouter.ai/api/v1";
	backends[1].api_key = "sk-or-...";
	backends[1].model = "anthropic/claude-3.5-sonnet";

	sicha_client_opts opts = { 0 };
	opts.struct_size = sizeof(opts);
	opts.backends = backends;
	opts.n_backends = 2;

	sicha_client *client = NULL;
	if (sicha_client_create(&opts, &client) != SICHA_OK) {
		return 1;
	}

	sicha_message msg = { SICHA_ROLE_USER, "Tell me a story.",
		SICHA_LEN_CSTR, NULL };
	sicha_request req = { 0 };
	req.struct_size = sizeof(req);
	req.messages = &msg;
	req.n_messages = 1;
	req.set_mask = SICHA_SET_TEMPERATURE | SICHA_SET_MAX_TOKENS;
	req.temperature = 0.8;
	req.max_tokens = 512;

	sicha_callbacks cbs = { 0 };
	cbs.struct_size = sizeof(cbs);
	cbs.on_delta = on_delta;

	sicha_result *result = NULL;
	sicha_status st = sicha_chat_stream(client, &req, &cbs, NULL,
		&result);
	if (st != SICHA_OK) {
		fprintf(stderr, "\nfailed: %s\n", sicha_status_str(st));
		for (uint32_t i = 0;
			i < sicha_result_attempt_count(result); i++) {
			const sicha_attempt *a =
				sicha_result_attempt(result, i);

			fprintf(stderr, "  attempt %u backend %u: %s\n",
				a->attempt, a->backend, a->message);
		}
	}
	printf("\n[finish: %s, %lld tokens]\n",
		sicha_finish_reason_str(sicha_result_finish_reason(result)),
		(long long)sicha_result_total_tokens(result));
	sicha_result_destroy(result);
	sicha_client_destroy(client);
	return st == SICHA_OK ? 0 : 1;
}
```

Compile with `-lsicha -lcurl` (or just `-lsicha` for a shared build).
Set `SICHA_DEBUG=-` for redacted wire logging to stderr.

## Testing your integration

The scripted transport mocks the entire wire deterministically:

```c
sicha_transport *script = sicha_script_create();
sicha_script_response resp = { 0 };
resp.struct_size = sizeof(resp);
resp.status = SICHA_T_OK;
resp.http_status = 200;
resp.body = "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},"
	"\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
resp.body_len = SICHA_LEN_CSTR;
resp.chunk_size = 3;             /* brutal TCP fragmentation */
sicha_script_push(script, &resp);

sicha_client_opts opts = { 0 };
/* ... backends as usual ... */
opts.transport = script;         /* no sockets involved */
```

Pair it with an injected `sicha_clock` (virtual time) and timeout /
backoff behavior becomes exactly testable with zero real waiting —
that is how sicha's own engine suites work.  Every scripted call is
recorded (`sicha_script_call_url/_body/_header`) for asserting what
was sent.

## Live-endpoint test (opt-in)

The suite includes a live integration test that runs the full client
against any real OAI-compatible endpoint.  It is inert until
configured — ctest reports it as `***Skipped` — and costs three tiny
requests when it runs (small `max_tokens`, the long generation is
cancelled after a few deltas):

```sh
SICHA_LIVE_BASE_URL=http://localhost:5001/v1 \
SICHA_LIVE_MODEL=whatever-is-loaded \
ctest --test-dir build -R live --output-on-failure
```

Optional: `SICHA_LIVE_API_KEY` (hosted endpoints),
`SICHA_LIVE_KOBOLD_ASSIST=1` (also exercise the KoboldCpp
`/api/extra/abort` cancellation assist).  It covers a blocking
completion, a streamed completion (callback bytes must equal the
accumulated result), and mid-stream cancellation.

## Demos

Two notcurses demos (`SICHA_BUILD_DEMO=ON`, off by default; needs
[notcurses](https://github.com/dankamongmen/notcurses) via pkg-config
or `-DSICHA_NOTCURSES_INCLUDE=... -DSICHA_NOTCURSES_LIB=...`):

- **`sicha_chat`** — a minimal streaming chat client: scrolling
  transcript, line editor, live deltas, Esc-to-cancel (with the
  KoboldCpp abort assist under `--kobold`), multi-turn history, and
  attempt telemetry in the status line.

  ```sh
  ./sicha_chat http://localhost:5001/v1 my-model --kobold
  ```

  Its threading pattern is the reference for GUI/TUI consumers: the
  blocking sicha call runs on a worker thread whose callbacks only
  fill a mutex-guarded handoff buffer; the UI thread drains it every
  frame and owns cancellation.  `--auto "message"` sends one message
  unattended and exits (pty smoke mode).

- **`sicha_fallback`** — the retry/fallback engine made visible, with
  NO network: the wire is the public scripted transport and you
  script it from the keyboard (`o` ok · `r` reset · `5` http 500 ·
  `9` 429 + Retry-After · `t` timeout · `m` malformed · `a` 401 ·
  ENTER fires, `c` cancels).  Watch classifications, jittered
  backoff, Retry-After waits, and chain advancement live.
  `--selftest` runs a canonical scenario headless (no tty).

`xxt/pty_demo.py` drives either demo in a pty for unattended checks —
it impersonates a terminal well enough for notcurses to boot
(answers the init probes, sets a real winsize), types scripted keys,
and asserts on the rendered output.

## Vendored / third-party

- [yyjson](https://github.com/ibireme/yyjson) 0.12.0 (MIT) is vendored
  as `src/yyjson.{c,h}` for JSON reading and writing.

## Roadmap

v0.2: streaming tool-call assembly, OpenRouter `GET /generation` cost
lookup, curl-share connection cache, KoboldCpp abort `genkey`.  Later:
a Raku NativeCall distribution.  (Tracked in the data-pipes monorepo,
`TODO/sicha.md`.)

## License

MIT.
