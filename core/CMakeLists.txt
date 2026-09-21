# libmynah — the engine behind a C API (docs/ENGINE-MIGRATION.md).
#
# Phase 1: the C API, config module, hallucination filter, constants.
# Phase 2: the engine — audio ring buffer + analysis, the utterance
# detector, Silero VAD, whisper.cpp STT, the session state machine, and
# the mynah-replay dev tool. Linux front ends (Phase 3+) link this.

add_library(mynah
    src/api/mynah.cpp
    src/audio/analysis.cpp
    src/config/config.cpp
    src/config/flat_toml.cpp
    src/filter/transcript_filter.cpp
    src/models/resolve.cpp
    src/models/table.cpp
    src/models/tiers.cpp
    src/segment/utterance_detector.cpp
    src/session/session.cpp
    src/stt/whisper_stt.cpp
    src/vad/silero.cpp
)
add_library(mynah::mynah ALIAS mynah)

# The only header front ends include, by contract: include/mynah/mynah.h.
target_include_directories(mynah PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Internal headers, relative to src/ — the Linux front end (linux/)
# links libmynah and reaches for models/, config/ and stt/ directly; they
# are not part of the C API and may change between releases.
target_include_directories(mynah PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(mynah PUBLIC cxx_std_20)

# MYNAH_VERSION is the project version from the root CMakeLists (P9) — the
# single version number, reported by mynah_version().
target_compile_definitions(mynah PRIVATE MYNAH_VERSION="${PROJECT_VERSION}")

# M6: whisper.cpp pinned and statically linked on every platform. Public:
# front ends linking libmynah get the whisper symbols the C API drives.
target_link_libraries(mynah PUBLIC whisper)

# Dev tool: a WAV through the full engine, events and transcripts out.
add_executable(mynah-replay tools/mynah-replay/main.cpp)
target_link_libraries(mynah-replay PRIVATE mynah)

if(MYNAH_BUILD_TESTS)
    add_executable(mynah_tests
        tests/main.cpp
        tests/test_api.cpp
        tests/test_audio.cpp
        tests/test_config.cpp
        tests/test_constants.cpp
        tests/test_detector.cpp
        tests/test_filter.cpp
        tests/test_fuzz.cpp
        tests/test_models.cpp
        tests/test_session.cpp
        tests/test_toml.cpp
    )
    target_link_libraries(mynah_tests PRIVATE mynah)
    target_include_directories(mynah_tests PRIVATE src tests ${CMAKE_CURRENT_SOURCE_DIR}/../linux/common)
    target_compile_definitions(mynah_tests PRIVATE
        MYNAH_VERSION="${PROJECT_VERSION}"
        # The suites pin the core's constants against the shared contract the
        # Python and Swift suites already pin — so they read the same file.
        MYNAH_TUNING_TOML="${CMAKE_CURRENT_SOURCE_DIR}/../tuning/tuning.toml"
        MYNAH_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tuning/golden"
    )
    add_test(NAME mynah_tests COMMAND mynah_tests)
endif()