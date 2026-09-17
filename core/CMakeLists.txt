# libmynah — the engine behind a C API (docs/ENGINE-MIGRATION.md).
#
# Phase 1: the C API header with stubs, the config module, the hallucination
# filter, and the constants ported from tuning/tuning.toml. Audio capture,
# segmentation, VAD and STT land in Phase 2 and grow into the src/ tree below
# without changing the header front ends compile against.

add_library(mynah
    src/api/mynah.cpp
    src/config/config.cpp
    src/config/flat_toml.cpp
    src/filter/transcript_filter.cpp
)
add_library(mynah::mynah ALIAS mynah)

# The only header front ends include.
target_include_directories(mynah PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Internal headers, included relative to src/.
target_include_directories(mynah PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(mynah PUBLIC cxx_std_20)

# MYNAH_VERSION is the project version from the root CMakeLists (P9) — the
# single version number, reported by mynah_version().
target_compile_definitions(mynah PRIVATE MYNAH_VERSION="${PROJECT_VERSION}")

if(TARGET whisper)
    # M6: whisper.cpp pinned and statically linked on every platform.
    target_link_libraries(mynah PUBLIC whisper)
endif()

if(MYNAH_BUILD_TESTS)
    add_executable(mynah_tests
        tests/main.cpp
        tests/test_api.cpp
        tests/test_toml.cpp
        tests/test_config.cpp
        tests/test_constants.cpp
        tests/test_filter.cpp
    )
    target_link_libraries(mynah_tests PRIVATE mynah)
    target_include_directories(mynah_tests PRIVATE src tests)
    target_compile_definitions(mynah_tests PRIVATE
        MYNAH_VERSION="${PROJECT_VERSION}"
        # The suites pin the core's constants against the shared contract the
        # Python and Swift suites already pin — so they read the same file.
        MYNAH_TUNING_TOML="${CMAKE_CURRENT_SOURCE_DIR}/../tuning/tuning.toml"
    )
    add_test(NAME mynah_tests COMMAND mynah_tests)
endif()