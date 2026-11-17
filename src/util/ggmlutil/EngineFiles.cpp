#include "EngineFiles.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <system_error>

#include <synthrt/Support/Error.h>

namespace otter::ggml {

    srt::Expected<void> writeWav(const std::filesystem::path &path,
                                 const std::vector<float> &samples, int sampleRate) {
        std::FILE *file = std::fopen(path.string().c_str(), "wb");
        if (file == nullptr) {
            return srt::Error(srt::Error::FileNotOpen, "cannot write " + path.string());
        }
        const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples.size() * sizeof(float));
        const std::uint32_t rate = static_cast<std::uint32_t>(sampleRate);
        struct {
            char riff[4];
            std::uint32_t riffSize;
            char wave[4];
            char fmt[4];
            std::uint32_t fmtSize;
            std::uint16_t format;
            std::uint16_t channels;
            std::uint32_t sampleRate;
            std::uint32_t byteRate;
            std::uint16_t blockAlign;
            std::uint16_t bits;
            char data[4];
            std::uint32_t dataSize;
        } header{};
        std::memcpy(header.riff, "RIFF", 4);
        header.riffSize = 36 + dataBytes;
        std::memcpy(header.wave, "WAVE", 4);
        std::memcpy(header.fmt, "fmt ", 4);
        header.fmtSize = 16;
        header.format = 3; // IEEE float
        header.channels = 1;
        header.sampleRate = rate;
        header.byteRate = rate * sizeof(float);
        header.blockAlign = sizeof(float);
        header.bits = 32;
        std::memcpy(header.data, "data", 4);
        header.dataSize = dataBytes;
        bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1;
        if (ok && !samples.empty()) {
            ok = std::fwrite(samples.data(), sizeof(float), samples.size(), file) == samples.size();
        }
        std::fclose(file);
        if (!ok) {
            return srt::Error(srt::Error::FileNotOpen, "cannot write " + path.string());
        }
        return srt::Expected<void>();
    }

    srt::Expected<std::filesystem::path> makeScratchDirectory(const std::string &name) {
        static std::atomic<unsigned long long> counter{0};
        const auto unique =
            std::chrono::steady_clock::now().time_since_epoch().count() * 31 + counter.fetch_add(1);
        auto root = std::filesystem::temp_directory_path() / name;
        root /= std::to_string(unique);
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error) {
            return srt::Error(srt::Error::FileNotOpen, "cannot create the scratch directory " +
                                                           root.string() + ": " + error.message());
        }
        return root;
    }

    void removeScratchDirectory(const std::filesystem::path &path) {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

}
