# The Linux side (docs/ENGINE-MIGRATION.md, Phase 3): the headless `mynah`
# binary, the shared Linux front-end code, and its tests.
#
# Structure mirrors the plan's layout:
#   common/   PipeWire capture, control socket, injectors, downloads, setup
#   cli/      the `mynah` binary
#   packaging/ is data, not build output (PKGBUILDs, the systemd unit)
#
# The socket, injectors, downloads, setup and systemd wrap only POSIX,
# libcurl and OpenSSL: they build anywhere, so the suite keeps compiling
# (and its tests keep running) on a dev Mac. Only PipeWire capture — and
# with it the `mynah` binary — is Linux-only.

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(mynah-linux-common STATIC
    common/control_socket.cpp
    common/downloads.cpp
    common/injector.cpp
    common/setup.cpp
    common/systemd.cpp
)
target_link_libraries(mynah-linux-common PUBLIC mynah)
target_include_directories(mynah-linux-common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(mynah-linux-common PRIVATE CURL::libcurl OpenSSL::Crypto)

# PipeWire capture, and with it the headless binary: Linux only.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3>=0.3)
endif()
if(PIPEWIRE_FOUND)
    target_sources(mynah-linux-common PRIVATE common/pipewire_capture.cpp)
    target_link_libraries(mynah-linux-common PUBLIC PkgConfig::PIPEWIRE)

    add_executable(mynah cli/main.cpp)
    target_link_libraries(mynah PRIVATE mynah-linux-common)
    install(TARGETS mynah DESTINATION bin)
endif()

if(MYNAH_BUILD_TESTS)
    add_executable(mynah-linux-tests
        tests/main.cpp
        tests/test_control_socket.cpp
        tests/test_downloads.cpp
        tests/test_injector.cpp
        tests/test_tiers.cpp
    )
    target_link_libraries(mynah-linux-tests PRIVATE mynah-linux-common) # carries libmynah
    target_include_directories(mynah-linux-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests
    )
    add_test(NAME mynah-linux-tests COMMAND mynah-linux-tests)
endif()