// mynah::setup — `mynah setup` (alias `doctor`), the read-only first-run
// checks (Phase 3: read-only now; the packages cover the dependencies).
// The port of preflight.py's Linux checks, minus the parts the C++ core
// retired: whisper.cpp is in the binary, so "is whisper-cli installed" is
// no longer a question — the model is.

#pragma once

#include <string>
#include <vector>

namespace mynah::setup {

struct Check {
    bool ok = false;
    std::string title;
    std::string detail; // one line
    std::string hint;   // multi-line remedy, printed only when !ok (or advisory)
};

std::vector<Check> run_checks();

// Print the results; returns 0 when every check passed, 1 otherwise.
int report(const std::vector<Check>& checks);

} // namespace mynah::setup