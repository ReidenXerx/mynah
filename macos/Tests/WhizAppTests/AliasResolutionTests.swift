import Foundation
import Testing
@testable import WhizApp

// The Swift resolver used to accept only literal paths and bare filenames,
// while `resolve()` in whiz/models.py accepts full aliases, short aliases and
// prefixes. The same `dictate_model` value therefore meant a working model on
// the Python side and "no model found" on the Swift side. These tests pin the
// parity (mirroring tests/test_models.py's resolve tests).

@Suite("Model alias resolution")
struct AliasResolutionTests {

    /// A temp directory with empty `.bin` files for the given model filenames.
    private func makeDisk(_ filenames: [String]) throws -> URL {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("whiz-alias-tests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        for name in filenames {
            FileManager.default.createFile(
                atPath: dir.appendingPathComponent(name).path, contents: Data())
        }
        return dir
    }

    @Test("resolves a full alias like large-v3-turbo-q5_0")
    func fullAlias() throws {
        let dir = try makeDisk(["ggml-large-v3-turbo-q5_0.bin"])
        defer { try? FileManager.default.removeItem(at: dir) }
        let resolved = WhisperModel.resolve(configured: "large-v3-turbo-q5_0", searchDirs: [dir])
        #expect(resolved?.lastPathComponent == "ggml-large-v3-turbo-q5_0.bin")
    }

    @Test("resolves a filename written in config as if it were an alias")
    func filenameNormalizesToAlias() throws {
        // The settings UI shows filenames (ggml-medium.bin); a user copying
        // that into the config must not get "no model found".
        let dir = try makeDisk(["ggml-medium.bin"])
        defer { try? FileManager.default.removeItem(at: dir) }
        let resolved = WhisperModel.resolve(configured: "ggml-medium.bin", searchDirs: [dir])
        #expect(resolved?.lastPathComponent == "ggml-medium.bin")
    }

    @Test("short alias 'turbo' resolves when exactly one turbo variant exists")
    func shortAliasUnique() throws {
        // Mirrors test_resolve_turbo_short_alias in tests/test_models.py.
        let dir = try makeDisk(["ggml-large-v3-turbo-q5_0.bin"])
        defer { try? FileManager.default.removeItem(at: dir) }
        let resolved = WhisperModel.resolve(configured: "turbo", searchDirs: [dir])
        #expect(resolved?.lastPathComponent == "ggml-large-v3-turbo-q5_0.bin")
    }

    @Test("short alias 'turbo' is ambiguous with two turbo variants — resolves to nothing, like Python")
    func shortAliasAmbiguous() throws {
        let dir = try makeDisk([
            "ggml-large-v3-turbo.bin",
            "ggml-large-v3-turbo-q5_0.bin",
        ])
        defer { try? FileManager.default.removeItem(at: dir) }
        #expect(WhisperModel.resolve(configured: "turbo", searchDirs: [dir]) == nil)
    }

    @Test("prefix 'large-v3' picks the best on-disk variant by preference order")
    func prefixPrefersUnquantized() throws {
        let dir = try makeDisk([
            "ggml-large-v3.bin",
            "ggml-large-v3-q5_0.bin",
        ])
        defer { try? FileManager.default.removeItem(at: dir) }
        let resolved = WhisperModel.resolve(configured: "large-v3", searchDirs: [dir])
        #expect(resolved?.lastPathComponent == "ggml-large-v3.bin")
    }

    @Test("prefix 'large-v3' resolves a quantized-only disk")
    func prefixQuantizedOnly() throws {
        let dir = try makeDisk(["ggml-large-v3-q5_0.bin"])
        defer { try? FileManager.default.removeItem(at: dir) }
        let resolved = WhisperModel.resolve(configured: "large-v3", searchDirs: [dir])
        #expect(resolved?.lastPathComponent == "ggml-large-v3-q5_0.bin")
    }

    @Test("prefix never rewrites an explicit quantized request")
    func prefixHonorsExplicitQuantized() throws {
        let dir = try makeDisk([
            "ggml-large-v3-turbo.bin",
            "ggml-large-v3-turbo-q5_0.bin",
        ])
        defer { try? FileManager.default.removeItem(at: dir) }
        let resolved = WhisperModel.resolve(configured: "large-v3-turbo-q5_0", searchDirs: [dir])
        #expect(resolved?.lastPathComponent == "ggml-large-v3-turbo-q5_0.bin")
    }

    @Test("an unknown name resolves to nothing, not to a fallback")
    func unknownIsNil() throws {
        let dir = try makeDisk(["ggml-large-v3.bin"])
        defer { try? FileManager.default.removeItem(at: dir) }
        #expect(WhisperModel.resolve(configured: "nonexistent-model", searchDirs: [dir]) == nil)
    }

    @Test("auto-pick (empty configured) walks the full five-class preference")
    func autoPickCoversAllClasses() throws {
        // The old list stopped at medium; a disk holding only small/base
        // models auto-picked nothing. Python's PREFERENCE has all eleven
        // entries, so pin entry-for-entry equality with the current list.
        #expect(WhisperModel.preference == [
            "ggml-large-v3-turbo.bin",
            "ggml-large-v3-turbo-q8_0.bin",
            "ggml-large-v3-turbo-q5_0.bin",
            "ggml-large-v3.bin",
            "ggml-large-v3-q5_0.bin",
            "ggml-medium.bin",
            "ggml-medium-q5_0.bin",
            "ggml-small.bin",
            "ggml-small-q5_0.bin",
            "ggml-base.bin",
            "ggml-base-q5_0.bin",
        ])
        let dir = try makeDisk(["ggml-small.bin"])
        defer { try? FileManager.default.removeItem(at: dir) }
        let resolved = WhisperModel.resolve(configured: "", searchDirs: [dir])
        #expect(resolved?.lastPathComponent == "ggml-small.bin")
    }

    @Test("alias(fromFilename:) mirrors _alias_from_name")
    func aliasHelper() {
        #expect(WhisperModel.alias(fromFilename: "ggml-large-v3-turbo-q5_0.bin")
                == "large-v3-turbo-q5_0")
        #expect(WhisperModel.alias(fromFilename: "ggml-medium.bin") == "medium")
        #expect(WhisperModel.alias(fromFilename: "foo.bin") == "foo")
        #expect(WhisperModel.alias(fromFilename: "ggml-foo") == "foo")
    }

    @Test("non-ggml and non-bin files are not discovered as aliases")
    func discoveryFilters() throws {
        let dir = try makeDisk([
            "ggml-medium.bin",
            "random.bin",        // no ggml- prefix
            "not-a-model.txt",   // not .bin
        ])
        defer { try? FileManager.default.removeItem(at: dir) }
        // Discovery resolves symlinks (/var → /private/var) when reading
        // directories; the expected URL does not. Normalize both sides so
        // the comparison is about the file, not the symlink expansion.
        let resolved = WhisperModel.resolve(configured: "medium", searchDirs: [dir])
        let expected = dir.appendingPathComponent("ggml-medium.bin")
        #expect(resolved?.standardizedFileURL.path == expected.standardizedFileURL.path)
        #expect(WhisperModel.resolve(configured: "random", searchDirs: [dir]) == nil)
        #expect(WhisperModel.resolve(configured: "not-a-model", searchDirs: [dir]) == nil)
    }
}