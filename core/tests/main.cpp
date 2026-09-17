// The one TU that implements doctest's main; every other file in tests/ just
// includes the vendored header (core/tests/vendor/doctest.h, doctest 2.4.12).
//
// Before any test runs, the config locations are pointed into a scratch
// directory of this run's own, so no test can read or write the developer's
// real files: MYNAH_CONFIG_DIR keeps ~/.config/mynah/config.toml out of reach,
// and MYNAH_LEGACY_CONFIG names a whiz config that does not exist, so the
// first-run import never pulls in ~/.config/whiz/config.toml. Tests that need
// either one set their own with EnvOverride, which restores these afterwards.
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
        fs::temp_directory_path() / ("mynah-core-tests-env-" + std::to_string(::getpid()));
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
