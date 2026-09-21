// A minimal RIFF/WAVE reader for 16 kHz mono s16le files — the format the
// golden corpus, the benchmark clip, and mynah-replay all use. Floats come
// out normalized to [-1, 1], the engine's sample domain.

#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace mynah::audio {

inline std::vector<float> read_wav_mono_16k(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    auto fail = [&](const std::string& why) {
        throw std::runtime_error(path.filename().string() + ": " + why);
    };
    if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) != 0 ||
        std::memcmp(data.data() + 8, "WAVE", 4) != 0)
        fail("not a RIFF/WAVE file");

    int sample_rate = 0;
    int channels = 0;
    std::vector<float> samples;
    std::size_t offset = 12;
    while (offset + 8 <= data.size()) {
        std::string id(reinterpret_cast<const char*>(data.data()) + offset, 4);
        std::size_t size = data[offset + 4] | (data[offset + 5] << 8) |
                           (std::size_t(data[offset + 6]) << 16) |
                           (std::size_t(data[offset + 7]) << 24);
        std::size_t start = offset + 8;
        if (id == "fmt " && size >= 16) {
            channels = data[start + 2] | (data[start + 3] << 8);
            sample_rate = data[start + 4] | (data[start + 5] << 8) |
                          (data[start + 6] << 16) | (data[start + 7] << 24);
        } else if (id == "data") {
            std::size_t end = std::min(start + size, data.size());
            samples.reserve((end - start) / 2);
            for (std::size_t i = start; i + 1 < end; i += 2) {
                std::uint16_t raw = data[i] | (data[i + 1] << 8);
                samples.push_back(float(std::int16_t(raw)) / 32768.0f);
            }
        }
        offset = start + size + (size % 2); // chunks are word-aligned
    }
    if (samples.empty()) fail("no PCM data");
    if (sample_rate != 16000) fail("expected 16 kHz, got " + std::to_string(sample_rate));
    if (channels != 1) fail("expected mono, got " + std::to_string(channels) + " channels");
    return samples;
}

} // namespace mynah::audio