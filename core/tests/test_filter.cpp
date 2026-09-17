// Pins NS-6 for the C++ filter — the mirror of
// macos/Tests/MynahAppTests/TranscriptFilterTests.swift and the filter cases
// in tests/test_dictate.py. The wave-1 audit found that vocabulary words
// ("перевод") were substring-matched, so real dictation containing one
// ("Отправь перевод на карту") was silently dropped — the CRITICAL. The two
// match modes are separate behaviours worth pinning independently of the
// phrase-list contents (test_constants.cpp pins those).

#include "vendor/doctest.h"

#include "filter/transcript_filter.hpp"

using mynah::filter::is_hallucination;

TEST_CASE("empty and whitespace transcripts are hallucinations") {
    CHECK(is_hallucination(""));
    CHECK(is_hallucination("   "));
    CHECK(is_hallucination("\n\t"));
}

TEST_CASE("normal dictation is never filtered") {
    CHECK(!is_hallucination("привет мир"));
    CHECK(!is_hallucination("Отправь перевод на карту"));
    CHECK(!is_hallucination("сделай перевод документа"));
    CHECK(!is_hallucination("субтитры к видео готовы"));
}

TEST_CASE("artifact phrases match by substring anywhere") {
    CHECK(is_hallucination("Спасибо за субтитры Алексею Дубровскому!"));
    CHECK(is_hallucination("продолжение следует..."));
    CHECK(is_hallucination("Видео подготовлено, amara.org"));
    CHECK(is_hallucination("ну и ... продолжение следует в следующей серии"));
}

TEST_CASE("vocabulary words match only as the whole utterance") {
    // The whole utterance IS the vocab word → hallucination.
    CHECK(is_hallucination("перевод"));
    CHECK(is_hallucination("  Перевод  "));
    CHECK(is_hallucination("субтитры"));
    CHECK(is_hallucination("корректор"));
}

TEST_CASE("vocab words inside longer speech pass") {
    CHECK(!is_hallucination("Отправь перевод на карту"));
    CHECK(!is_hallucination("открой субтитры"));
    CHECK(!is_hallucination("вызови корректора"));
}

TEST_CASE("matching is case-insensitive and trim-normalized") {
    CHECK(is_hallucination("  СПАСИБО ЗА СУБТИТРЫ  "));
    CHECK(is_hallucination("Перевод"));
    // Case-insensitive substring for artifacts.
    CHECK(is_hallucination("...ПРОДОЛЖЕНИЕ СЛЕДУЕТ..."));
    // And the vocab whole-equality survives normalization too.
    CHECK(!is_hallucination("  Отправь перевод на карту.  "));
}