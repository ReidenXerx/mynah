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
    common/paste_typer.cpp
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
# Typing on KWin (docs/LINUX-KDE-PLAN.md, Stage 3): a Wayland client of its
# own, over KWin's fake-input and ext-data-control protocols, generated
# here from the distro's protocol XML. Required on Linux — without it a
# Plasma session cannot type at all (KWin has no virtual keyboard for
# wtype) — unless switched off on purpose.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    option(MYNAH_KWIN "Build the KWin typer (Plasma)" ON)
else()
    set(MYNAH_KWIN OFF)
endif()
if(MYNAH_KWIN)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
        pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols)
    endif()
    find_package(PlasmaWaylandProtocols QUIET)
    find_program(WAYLAND_SCANNER wayland-scanner)
    if(NOT WAYLAND_CLIENT_FOUND OR NOT WAYLAND_PROTOCOLS_FOUND OR NOT PlasmaWaylandProtocols_FOUND
       OR NOT WAYLAND_SCANNER)
        message(FATAL_ERROR
            "The KWin typer needs libwayland-client, wayland-scanner and the protocol XML.\n"
            "  Arch:  sudo pacman -S wayland wayland-protocols plasma-wayland-protocols\n"
            "Or build without typing on Plasma: -DMYNAH_KWIN=OFF")
    endif()
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    set(protocol_xml
        "${WAYLAND_PROTOCOLS_DIR}/staging/ext-data-control/ext-data-control-v1.xml"
        "${PLASMA_WAYLAND_PROTOCOLS_DIR}/fake-input.xml")
    set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/wayland")
    file(MAKE_DIRECTORY "${generated_dir}")
    foreach(xml IN LISTS protocol_xml)
        get_filename_component(name "${xml}" NAME_WE)
        add_custom_command(
            OUTPUT "${generated_dir}/${name}-client-protocol.h" "${generated_dir}/${name}-protocol.c"
            COMMAND "${WAYLAND_SCANNER}" client-header "${xml}" "${generated_dir}/${name}-client-protocol.h"
            COMMAND "${WAYLAND_SCANNER}" private-code "${xml}" "${generated_dir}/${name}-protocol.c"
            DEPENDS "${xml}"
            VERBATIM)
        target_sources(mynah-linux-common PRIVATE
            "${generated_dir}/${name}-client-protocol.h" "${generated_dir}/${name}-protocol.c")
    endforeach()
    target_sources(mynah-linux-common PRIVATE common/kwin_backend.cpp)
    target_include_directories(mynah-linux-common PRIVATE "${generated_dir}")
    target_link_libraries(mynah-linux-common PRIVATE PkgConfig::WAYLAND_CLIENT)
    target_compile_definitions(mynah-linux-common PUBLIC MYNAH_HAVE_KWIN_TYPER=1)
endif()

if(PIPEWIRE_FOUND)
    target_sources(mynah-linux-common PRIVATE common/pipewire_capture.cpp common/runtime.cpp)
    target_link_libraries(mynah-linux-common PUBLIC PkgConfig::PIPEWIRE)

    # The target is `mynah-cli` because `mynah` is the library's
    # (core/CMakeLists.txt); the file it builds is still `mynah`.
    add_executable(mynah-cli cli/main.cpp)
    set_target_properties(mynah-cli PROPERTIES OUTPUT_NAME mynah)
    target_link_libraries(mynah-cli PRIVATE mynah-linux-common)
    target_compile_definitions(mynah-cli PRIVATE MYNAH_VERSION="${PROJECT_VERSION}")
    install(TARGETS mynah-cli DESTINATION bin)
endif()

# mynah-kde, the Plasma front end (Stage 8). On by default on Linux, where
# the package builds it; -DMYNAH_KDE=OFF for a headless-only build (the
# Omarchy package). It needs the runtime, so PipeWire too.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND PIPEWIRE_FOUND)
    option(MYNAH_KDE "Build mynah-kde, the Plasma front end" ON)
    if(MYNAH_KDE)
        add_subdirectory(kde)
    endif()
endif()

if(MYNAH_BUILD_TESTS)
    add_executable(mynah-linux-tests
        tests/main.cpp
        tests/test_control_socket.cpp
        tests/test_downloads.cpp
        tests/test_injector.cpp
        tests/test_tiers.cpp
        tests/test_typing_queue.cpp
        tests/test_paste_typer.cpp
    )
    target_link_libraries(mynah-linux-tests PRIVATE mynah-linux-common) # carries libmynah
    target_include_directories(mynah-linux-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/common
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests
    )
    if(PIPEWIRE_FOUND)
        target_sources(mynah-linux-tests PRIVATE tests/test_capture.cpp)
    endif()
    add_test(NAME mynah-linux-tests COMMAND mynah-linux-tests)
    if(MYNAH_SANITIZE STREQUAL "address")
        set_tests_properties(mynah-linux-tests PROPERTIES ENVIRONMENT
            "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
    endif()
endif()