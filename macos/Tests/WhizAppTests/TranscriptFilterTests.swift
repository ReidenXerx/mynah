import Testing
import Foundation
@testable import WhizApp

// Pins NS-6 for the Swift filter — the mirror of the engine.py tests in
// tests/test_dictate.py. The wave-1 audit found that vocabulary words
// ("перевод") were substring-matched, so real dictation containing one
// ("Отправь перевод на карту") was silently dropped — the CRITICAL. The
// two match modes are separate behaviors worth pinning independently
// of the phrase-list contents (TuningTests pins those).

@Suite("TranscriptFilter hallucination matching")
struct TranscriptFilterTests {

    @Test("empty and whitespace transcripts are hallucinations")
    func emptyIsHallucination() {
        #expect(TranscriptFilter.isHallucination(""))
        #expect(TranscriptFilter.isHallucination("   "))
        #expect(TranscriptFilter.isHallucination("\n\t"))
    }

    @Test("normal dictation is never filtered")
    func normalSpeechPasses() {
        #expect(!TranscriptFilter.isHallucination("привет мир"))
        #expect(!TranscriptFilter.isHallucination("Отправь перевод на карту"))
        #expect(!TranscriptFilter.isHallucination("сделай перевод документа"))
        #expect(!TranscriptFilter.isHallucination("субтитры к видео готовы"))
    }

    @Test("artifact phrases match by substring anywhere")
    func artifactsMatchBySubstring() {
        #expect(TranscriptFilter.isHallucination("Спасибо за субтитры Алексею Дубровскому!"))
        #expect(TranscriptFilter.isHallucination("продолжение следует..."))
        #expect(TranscriptFilter.isHallucination("Видео подготовлено, amara.org"))
        #expect(TranscriptFilter.isHallucination("ну и ... продолжение следует в следующей серии"))
    }

    @Test("vocabulary words match only as the whole utterance")
    func vocabMatchesOnlyWholeUtterance() {
        // The whole utterance IS the vocab word → hallucination.
        #expect(TranscriptFilter.isHallucination("перевод"))
        #expect(TranscriptFilter.isHallucination("  Перевод  "))
        #expect(TranscriptFilter.isHallucination("субтитры"))
        #expect(TranscriptFilter.isHallucination("корректор"))
    }

    @Test("vocab words inside longer speech pass")
    func vocabInsideLongerSpeechPasses() {
        #expect(!TranscriptFilter.isHallucination("Отправь перевод на карту"))
        #expect(!TranscriptFilter.isHallucination("открой субтитры"))
        #expect(!TranscriptFilter.isHallucination("вызови корректора"))
    }

    @Test("matching is case-insensitive and trim-normalized")
    func normalizationIsApplied() {
        #expect(TranscriptFilter.isHallucination("  СПАСИБО ЗА СУБТИТРЫ  "))
        #expect(TranscriptFilter.isHallucination("Перевод"))
        // Case-insensitive substring for artifacts.
        #expect(TranscriptFilter.isHallucination("...ПРОДОЛЖЕНИЕ СЛЕДУЕТ..."))
        // And the vocab whole-equality survives normalization too.
        #expect(!TranscriptFilter.isHallucination("  Отправь перевод на карту.  "))
    }
}