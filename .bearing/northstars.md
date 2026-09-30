# North-stars — mynah

Falsifiable propositions about what this project IS. This file outranks every
other doc (README.md, docs/SWIFT-APP.md, docs/LINUX-APP.md, code comments) — on
conflict, the north-star wins and the other source is stale. Owned by the user;
agents propose diffs, never edit silently.

**Numbering provenance.** These stars were ported from the pre-split whiz
north-stars (state at whiz commit `958fe6b^`, i.e. whiz's final pre-rewrite
northstars.md) when the segmentation contract moved to this repo at the split
(whiz commit `a92c8d3`, 2026-09). The OLD numbering is preserved on purpose:
mynah code and docs already cite these numbers by name — `NS-6` (engine.py,
TranscriptFilter.swift, TranscriptFilterTests.swift, tests/test_dictate.py,
docs/SWIFT-APP.md) and `NS-15` (WhisperModel.swift, WhisperModelTests.swift,
ModelDownloader.swift, ModelSectionView.swift, docs/SWIFT-APP.md; the C++ core's
core/src/models/resolve.hpp and core/tests/* pin the same contracts). Renumbering
would orphan every one of those citations. whiz's own north-stars were
renumbered at `958fe6b`; the cross-repo mapping is: old NS-1..NS-8, NS-9..NS-15
→ mynah NS-1..NS-15 (same numbers, same meanings, re-pointed paths).

**Scope.** These are ENGINE-level invariants about the segmentation/energy-gate
/hallucination-filter pipeline. They are deliberately
implementation-agnostic — Python (`mynah/engine.py`), Swift
(`macos/Sources/MynahApp/...`), and the C++ core (`core/src/...`,
feature/cpp-core-migration) must all hold them; a language rework changes the
code, never these contracts. Stars that stayed with whiz (diarization-cache
keying, degraded-run loudness, config-key preservation, voice-profile
provenance, nearest-in-time utterance assignment) are NOT ported — those live
in whiz's northstars.md on its new numbering.

## Invariants — must always hold

- **NS-1** — Every implementation (Python engine, Swift app, C++ core, future
  ports) compiles the same segmentation constants, pinned against
  `tuning/tuning.toml` by that implementation's test suite; drift is a build
  failure, never a silent behavior change. — src: tests/test_tuning.py,
  macos/Tests/MynahAppTests/TuningTests.swift, core/tests/test_constants.cpp
  (C++ branch), tuning/tuning.toml:1-26
- **NS-2** — Every golden corpus case yields identical speech-region
  boundaries and energy-gate verdict across implementations, and the verdict is
  invariant under both trailing-silence policies; the generator refuses to emit
  a corpus where they differ (raises `SystemExit` rather than pinning a case the
  policies disagree on). — src: tuning/golden/generate.py:230-246,
  tests/test_segmentation_golden.py, macos/Tests/MynahAppTests/TuningTests.swift
- **NS-3** — A tuning change lands as one commit: `tuning.toml` + every pinning
  test + regenerated corpus; corpus regeneration is byte-identical and
  test-enforced. — src: tests/test_tuning.py (test_golden_corpus_is_regenerable,
  test_golden_generator_constants_match_tuning), tuning/golden/generate.py:14-18

## Semantics — exact meaning here

- **NS-4** — `utterance end` = start time of the first closing silent frame;
  buffered PCM spans [start, end + frame); Swift's 0.2 s trim removes from the
  tail of that span. NOT "last speech frame", NOT "buffer end". — src:
  tuning/golden/expected.json, tuning/golden/generate.py:22-29,
  tests/test_segmentation_golden.py docstring
- **NS-5** — `rejected_by_energy_gate` = whole-buffer RMS below the calibrated
  (or default) utterance gate. NOT "VAD found no speech", NOT "transcription
  failed". — src: tuning/golden/expected.json, mynah/engine.py,
  tuning/golden/generate.py:207-213
- **NS-6** — Hallucination filtering is a hybrid match on the trimmed,
  lowercased transcript, per the two arrays in `tuning.toml`:
  `hallucination_artifact_phrases` (distinctive boilerplate) match by SUBSTRING;
  `hallucination_vocab_phrases` (ordinary vocabulary: "субтитры", "перевод",
  "корректор") match only when the whole transcript EQUALS the phrase. NOT
  substring matching for vocabulary (that silently dropped real speech like
  "Отправь перевод на карту" — the wave-1 CRITICAL), NOT fuzzy matching. — src:
  tuning/tuning.toml:110-159, mynah/engine.py:129, macos/Sources/MynahApp/STT/TranscriptFilter.swift,
  core/src/filter/transcript_filter.cpp (C++ branch)

## Evidence — what counts as proof

- **NS-7** — A segmentation-behavior claim requires a corpus run (existing case
  or a new committed case); reasoning about the state machine from source is NOT
  evidence. — src: tuning/golden/generate.py:85-86,
  tests/test_segmentation_golden.py docstring
- **NS-8** — "Tests pass" claims must name the command actually run — Python:
  `uv run --extra test --frozen pytest tests/ -q` (or the equivalent venv
  invocation); Swift: `swift test` under the sandbox-exec CLT-deny workaround on
  dual-toolchain machines; C++: the core's doctest runner via CTest; a bare
  claim is NOT evidence. — src: pyproject.toml [tool.pytest.ini_options],
  core/CMakeLists.txt, local toolchain state

## Settled — decided, do not relitigate

- **NS-9** — `tuning.toml` is data, never read at runtime — the runtime-read
  alternative was rejected because an absent/edited file yields silently
  inconsistent cross-platform behavior instead of a build failure. Each
  implementation compiles its constants in; the file is the contract test
  suites pin against. — src: tuning/tuning.toml:13-17,
  core/src/tuning/constants.hpp (C++ branch)
- **NS-10** — Linux support is Wayland-only; X11 has no security story for
  global hotkey + text injection — do not propose X11 backends. — src:
  docs/LINUX-APP.md, linux/ (C++ branch)
- **NS-11** — Known divergences (min-utterance gate placement, trailing-silence
  policy, secondary VAD, mlx's `hallucination_silence_threshold=2.0` which
  whisper.cpp has no equivalent for) are deliberate and unpinned; aligning one
  requires a decision + corpus update, not a quiet code change. — src:
  tuning/tuning.toml:78-95, mynah/providers/mlx.py:125-145,
  tests/test_segmentation_golden.py docstring
- **NS-15** — Whisper models are used UNQUANTIZED, always and everywhere —
  quantization corrupts transcription quality (user decision, 2026-09-03;
  ea49da8's 4-bit-turbo garbling is the recorded evidence, and the user extends
  it to all quantized variants). Defaults must prefer the unquantized variant of
  each class; a quantized variant is reachable once its OWN class's unquantized
  model is absent — never blocked behind unrelated unquantized classes
  (preference is PER-CLASS, user + review decision, 2026-09-05: classes rank
  large-v3-turbo > large-v3 > medium > small > base). `tiny` is excluded from
  the preference tables entirely (user decision, 2026-09-05: useless quality);
  it still downloads/runs when named explicitly. Engine-migration testing of a
  quantized model (e.g. q8_0 for `small`) is allowed as a phase-0 equivalence
  check, not a default. — src: macos/Sources/MynahApp/STT/WhisperModel.swift,
  macos/Sources/MynahApp/STT/ModelDownloader.swift,
  core/src/models/resolve.hpp (C++ branch), docs/SWIFT-APP.md:270,352

## Graveyard — tried and rejected / validated

- **NS-12** — REJECTED: runtime tuning-file dependency (see NS-9). Re-propose
  only with an answer to "what happens when the file is missing mid-session".
- **NS-13** — VALIDATED: golden corpus driven through the real
  callback/detector, not a reimplementation — it caught the FlatTOML
  multi-line array drop and the shared poisoned-calibration defect. Don't
  replace with mock-level tests. — src: tests/test_segmentation_golden.py,
  core/tests/golden.hpp (C++ branch)
- **NS-14** — RESOLVED: poisoned calibration — speech filling the 1 s window
  drove the utterance gate to ≈3× speech RMS and silently dropped the first
  word in BOTH implementations. Fixed by speech-aware calibration (calibration
  frames ≥ `calibration_speech_floor` (0.03) are excluded from the median;
  fewer than `noise_min_samples` (5) quiet frames aborts calibration to the
  static gates) and capped since wave-2 M13 (the median's contribution to the
  effective gates never rises above `calibration_speech_floor`, so a
  calibrated gate can never demand speech louder than speech). SCOPE: the fix
  covers speech at/above `calibration_speech_floor` (0.03) — speech BELOW the
  floor can still enter the median and poison it; that residual is ACCEPTED (an
  absolute floor trades a quiet-speech hole for speech/noise discrimination).
  Corpus cases `speech_during_calibration` (rejected_by_energy_gate: false) and
  `speech_over_noise_in_calibration` pin the fix and the exclusion mechanism.
  Trade-off: steady noise ≥ 0.03 RMS is no longer calibratable (VAD +
  hallucination filter own it, best-effort in Python). — src:
  tuning/tuning.toml:43-76, tuning/golden/generate.py:184-205, mynah/engine.py,
  core/src/segment/utterance_detector.cpp (C++ branch)

## Open — explicitly unresolved (do NOT assume either way)

(none)