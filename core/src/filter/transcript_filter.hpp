// mynah::filter — rejects Whisper's known failure output before it reaches
// the user's keyboard.
//
// The phrase list is the shared contract in tuning/tuning.toml, pinned by
// core/tests/test_constants.cpp; the match modes are pinned by
// core/tests/test_filter.cpp (ported from TranscriptFilterTests.swift). Both
// lists are lowercased and matched against the trimmed, lowercased
// transcript:
//
//   artifact_phrases — distinctive training-data boilerplate, matched by
//     SUBSTRING: not plausible real dictation anywhere in an utterance.
//   vocab_phrases — ordinary single words ("перевод") that ARE plausible
//     real dictation, matched only when the whole transcript EQUALS the
//     phrase. Substring matching there silently dropped real speech
//     ("Отправь перевод на карту" vanished); the wave-1 audit recorded it
//     as a CRITICAL.
//
// Caveat carried over from TranscriptFilter.swift: whisper.cpp reimplemented
// Whisper's decoding loop independently of the reference implementation the
// phrases were collected from, so the exact artifacts it emits on silence
// may differ. This is a starting point, not a guarantee — add to it as
// testing turns up new ones.
//
// Caveat specific to the C++ port: lowercasing covers ASCII, Latin-1 and
// Cyrillic — every script the phrase list and its tests use. Python's
// str.lower() and Swift's lowercased() are fully Unicode-aware; other
// scripts pass through unchanged here. Whisper transcripts in practice are
// Latin or Cyrillic, and a character the lowercaser misses only weakens
// matching (a phrase is not matched), never drops real speech.

#pragma once

#include <array>
#include <string_view>

namespace mynah::filter {

// hallucination_artifact_phrases from tuning/tuning.toml.
// Additional artifacts observed on MacBook cooler/fan noise at the tail.
inline constexpr std::array<std::string_view, 18> artifact_phrases{
    "спасибо за субтитры",
    "субтитры создавал",
    "субтитры выполнил",
    "субтитры делал",
    "субтитры подготовил",
    "редактор субтитров",
    "продолжение следует",
    "спасибо за просмотр",
    "спасибо за внимание",
    "подписывайтесь на канал",
    "by follows",
    "by following",
    "amara.org",
    "расскажите о себе",
    "следите за обновлениями",
    "оставайтесь с нами",
    "не забудьте подписаться",
    "вы можете поддержать",
};

// hallucination_vocab_phrases — ordinary vocabulary, whole-utterance equality
// only. A dictation containing one of these words as part of a longer phrase
// must pass.
inline constexpr std::array<std::string_view, 3> vocab_phrases{
    "субтитры",
    "перевод",
    "корректор",
};

// Whether transcribed text is a hallucination and should be dropped.
bool is_hallucination(std::string_view text);

} // namespace mynah::filter