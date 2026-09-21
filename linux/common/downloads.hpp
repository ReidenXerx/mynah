// mynah::download — fetching model files with libcurl (P8's front-end half
// on Linux). The core publishes URL/size/SHA-256 (models/table.hpp); this
// fetches, checks the HTTP status (a 404 body is a perfectly valid file),
// verifies the hash when the table has one, and lands the file atomically:
// the download goes to `name.part` and is renamed only after it passed.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace mynah::download {

// Called with (received_bytes, total_bytes_or_zero); return false to
// cancel.
using Progress = std::function<bool(std::uint64_t, std::uint64_t)>;

struct Result {
    bool ok = false;
    std::string path;   // where it landed on success
    std::string error;  // human wording on failure
};

// Download `url` into `directory` as `filename`, verifying `sha256` when
// it is not empty. Throws nothing; check Result.
Result fetch(const std::string& url, const std::string& directory,
             const std::string& filename, const std::string& sha256,
             const Progress& progress = {});

// The SHA-256 of a file, hex; empty on an I/O error. Used for verification
// and by `mynah models list` to say what is actually on disk.
std::string file_sha256(const std::string& path);

} // namespace mynah::download