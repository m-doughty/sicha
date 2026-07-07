# Changelog

## 0.1.0 — 2026-07-06

Initial release.

- OpenAI-compatible chat completion (`POST {base}/chat/completions`),
  blocking and streaming (SSE), against an ordered multi-backend
  fallback chain.
- Retry engine ported from LLM::Data::Inference::Task: abort /
  retry-same / advance classification (overridable per HTTP status),
  exponential backoff with jitter, Retry-After support, validation
  re-rolls, per-attempt records with body excerpts, and a per-round-
  trip telemetry callback.
- Streaming commit rule: delta bytes delivered to a callback make the
  stream non-retryable (`SICHA_E_STREAM_LOST`) unless
  `SICHA_POLICY_RETRY_AFTER_DELTAS` opts back in.
- Hardened incremental SSE parser (split-anywhere, CRLF/LF/CR, BOM,
  comments, multi-line data, lenient EOF) with partition-invariance
  property tests and a dedicated fuzzer.
- Transport vtable: built-in libcurl transport (HTTP/1.1 forced,
  four timeout classes — connect / first-byte / idle / total — with
  `SICHA_INFINITE` support, pooled handles for connection reuse) and a
  public scripted in-memory transport for deterministic tests.
- Thread-safe cancel tokens (instant condvar wake, mass cancel) and a
  KoboldCpp `/api/extra/abort` cancellation assist.
- Injectable clock + PRNG seed: exactly reproducible retry schedules.
- Safety rails: strict UTF-8 validation, CR/LF/NUL header-injection
  rejection, response/event/header size caps, redacted `SICHA_DEBUG`
  wire logging, overflow-checked buffers.
- Vendored yyjson 0.12.0 (MIT) for JSON.
- Two notcurses demos (SICHA_BUILD_DEMO): `sicha_chat`, a streaming
  chat client with cancellation and attempt telemetry (the reference
  worker-thread + handoff pattern for UI consumers), and
  `sicha_fallback`, a keyboard-scripted visualization of the retry /
  fallback engine over the public scripted transport (headless
  `--selftest` included), plus `xxt/pty_demo.py` for driving either
  unattended in a pty.
- Infrastructure: 14 ctest suites (~8,000 checks) including loopback
  HTTP/1.1 integration tests and an opt-in live-endpoint test
  (SICHA_LIVE_BASE_URL / SICHA_LIVE_MODEL / SICHA_LIVE_API_KEY;
  skipped visibly when unconfigured), ASan+UBSan and TSan lanes, four
  libFuzzer harnesses with committed corpora + nightly campaign,
  3-OS bitwise bench counter gate, 5-platform CI (Linux x86_64 gcc +
  clang, Linux aarch64, macOS arm64, Windows x86_64 MSVC).
