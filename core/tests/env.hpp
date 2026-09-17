// Shared test scaffolding: scoped environment overrides and fresh temp dirs.
// doctest runs cases serially in one process, which is what makes env
// mutation safe here.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace mynah_test {

// Restores the previous value (or absence) of the variable on destruction.
class EnvOverride {
public:
    EnvOverride(const char* name, std::string value)
        : name_(name), had_(std::getenv(name) != nullptr),
          old_(had_ ? std::getenv(name) : "") {
        if (setenv(name_.c_str(), value.c_str(), 1) != 0) std::abort();
    }
    ~EnvOverride() {
        if (had_) setenv(name_.c_str(), old_.c_str(), 1);
        else unsetenv(name_.c_str());
    }
    EnvOverride(const EnvOverride&) = delete;
    EnvOverride& operator=(const EnvOverride&) = delete;

private:
    std::string name_;
    bool had_;
    std::string old_;
};

// A fresh directory per construction; removed on destruction.
class TmpDir {
public:
    TmpDir() {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("mynah-core-tests-" + std::to_string(::getpid()) + "-" +
                 std::to_string(++counter));
        std::filesystem::create_directories(path_);
    }
    ~TmpDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TmpDir(const TmpDir&) = delete;
    TmpDir& operator=(const TmpDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline void write_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace mynah_test