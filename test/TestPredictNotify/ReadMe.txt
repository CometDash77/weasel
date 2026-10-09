TestPredictNotify
=================

S3 completion-notify receiver contract tests (RIME-PredictInput issue #22).

Instantiates the real ServerImpl window (class WeaselIPCWindow_1.0) with a
recording request handler and drives WM_COPYDATA against it:

- byte-exact payload delivery (engine_id / request_id / seq, UTF-8 + NUL,
  cbData counting the NUL, x64 COPYDATASTRUCT 24 bytes via static_assert)
- malformed rejection at the transport layer (missing/embedded/trailing
  NUL, wrong segment count, hex and seq grammar violations, size bounds)
- identity gating against the published request identity (stale, current,
  duplicate-of-current, unpublished, mismatched)
- failure silence for our COPYDATA id and fall-through for foreign dwData

Exit code 0 = all checks passed. When a real WeaselServer holds the
single-instance mutex, window-level cases are skipped.

Build with the weasel.sln Debug configuration (test project) or xmake in
debug mode. Requires the same environment as the main build (boost,
prebuilt librime mapped by get-rime.ps1).
