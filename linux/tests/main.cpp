// The one TU that implements doctest's main for the Linux suite; every
// other file in tests/ just includes the vendored header.
//
// Sandboxed like core/tests/main.cpp: before any test runs, the config
// locations point into a scratch directory of this run's own, so no test can
// read or write ~/.config/mynah/config.toml or import ~/.config/whiz.
#define DOCTEST_CONFIG_IMPLEMENT
#include "vendor/doctest.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include <unistd.h>

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path scratch =
        fs::temp_directory_path() / ("mynah-linux-tests-env-" + std::to_string(::getpid()));
    fs::create_directories(scratch / "config");
    setenv("MYNAH_CONFIG_DIR", (scratch / "config").c_str(), 1);
    setenv("MYNAH_LEGACY_CONFIG", (scratch / "no-whiz-config.toml").c_str(), 1);

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    int result = context.run();

    std::error_code ec;
    fs::remove_all(scratch, ec);
    return result;
}
