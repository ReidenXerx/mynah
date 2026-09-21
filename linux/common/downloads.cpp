#include "downloads.hpp"

#include <openssl/evp.h>

#include <curl/curl.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>

namespace mynah::download {

namespace {

// Everything the transfer callbacks need, behind ONE pointer: libcurl
// hands each callback exactly the userdata registered for it, and the body
// callback used to be registered with the FILE* while it read a
// FetchContext — so it poked the stdio struct, called a "progress" function
// that was really libc's bookkeeping, and wrote no bytes at all.
struct FetchContext {
    Progress progress;
    std::FILE* sink = nullptr;
    std::uint64_t received = 0;
    std::uint64_t total = 0;
    bool cancelled = false;
    bool write_failed = false; // disk full, or the file went away
};

void header_total(const char* header, std::uint64_t& total) {
    constexpr std::string_view prefix = "Content-Length:";
    std::string_view line(header);
    if (line.rfind(prefix, 0) == 0) {
        line.remove_prefix(prefix.size());
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        std::uint64_t value = 0;
        if (std::from_chars(line.data(), line.data() + line.size(), value).ec ==
            std::errc{})
            total = value;
    }
}

} // namespace

std::string file_sha256(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    // The EVP interface, not the SHA256_* one-shots: OpenSSL 3 and Apple's
    // LibreSSL both deprecate those.
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) return {};
    std::string hex;
    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1) {
        std::array<char, 65536> buffer{};
        while (in) {
            in.read(buffer.data(), std::streamsize(buffer.size()));
            if (in.gcount() > 0)
                EVP_DigestUpdate(context, buffer.data(), std::size_t(in.gcount()));
        }
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        std::uint32_t size = 0;
        if (EVP_DigestFinal_ex(context, digest.data(), &size) == 1) {
            static const char* digits = "0123456789abcdef";
            hex.reserve(std::size_t(size) * 2);
            for (std::uint32_t i = 0; i < size; ++i) {
                hex += digits[digest[i] >> 4];
                hex += digits[digest[i] & 0xF];
            }
        }
    }
    EVP_MD_CTX_free(context);
    return hex;
}

namespace {

size_t on_header(char* data, size_t size, size_t count, void* user) {
    auto* context = static_cast<FetchContext*>(user);
    header_total(data, context->total); // Content-Length, when present
    return size * count;
}

size_t on_body(char* data, size_t size, size_t count, void* user) {
    auto* context = static_cast<FetchContext*>(user);
    const size_t bytes = size * count;
    // A short write is a failed transfer: returning less than we were given
    // tells libcurl to abort with CURLE_WRITE_ERROR.
    if (std::fwrite(data, 1, bytes, context->sink) != bytes) {
        context->write_failed = true;
        return 0;
    }
    context->received += bytes;
    if (context->progress && !context->progress(context->received, context->total)) {
        context->cancelled = true;
        return 0; // abort the transfer
    }
    return bytes;
}

} // namespace

Result fetch(const std::string& url, const std::string& directory,
             const std::string& filename, const std::string& sha256,
             const Progress& progress) {
    Result result;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    std::filesystem::path destination = std::filesystem::path(directory) / filename;
    std::filesystem::path partial = destination;
    partial += ".part";

    FetchContext context;
    context.progress = progress;
    // file:// sources carry no HTTP status; they exist so the downloader can
    // be exercised without a network (the tests, an offline mirror).
    const bool local_source = url.rfind("file://", 0) == 0;

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "cannot initialize curl";
        return result;
    }
    std::FILE* sink = std::fopen(partial.c_str(), "wb");
    if (!sink) {
        curl_easy_cleanup(curl);
        result.error = "cannot write " + partial.string();
        return result;
    }
    context.sink = sink;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // HuggingFace redirects to a CDN
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L); // a stalled transfer…
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L); // …for a minute is dead

    CURLcode code = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);
    // fclose flushes; a failure here is data that never reached the disk.
    if (std::fclose(sink) != 0) context.write_failed = true;

    // A 404 body is a perfectly valid file: check the status before the
    // hash, and never let it land under the model's name.
    auto fail = [&](std::string message) {
        std::filesystem::remove(partial, ec);
        result.error = std::move(message);
        return result;
    };
    if (code == CURLE_ABORTED_BY_CALLBACK || context.cancelled) {
        result.ok = false;
        result.error = "cancelled";
        std::filesystem::remove(partial, ec);
        return result;
    }
    if (context.write_failed)
        return fail("cannot write " + partial.string() + " — is the disk full?");
    if (code != CURLE_OK) return fail("download failed: " + std::string(curl_easy_strerror(code)));
    if (!local_source && (http_status < 200 || http_status >= 300))
        return fail("server returned HTTP " + std::to_string(http_status));
    // The size check the model table promises when there is no hash to
    // check: a transfer that ended early is a truncated model, and whisper
    // would fail on it much later with a far less helpful message.
    if (context.total > 0 && context.received != context.total)
        return fail("download incomplete: got " + std::to_string(context.received) +
                    " of " + std::to_string(context.total) + " bytes");
    if (context.received == 0) return fail("download was empty");
    if (!sha256.empty()) {
        std::string actual = file_sha256(partial.string());
        if (actual != sha256)
            return fail("SHA-256 mismatch: expected " + sha256 + ", got " + actual);
    }

    std::filesystem::rename(partial, destination, ec);
    if (ec) {
        std::filesystem::remove(partial, ec);
        result.error = "cannot move the download into place: " + ec.message();
        return result;
    }
    result.ok = true;
    result.path = destination.string();
    return result;
}

} // namespace mynah::download