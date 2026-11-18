#include <system_error>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <iterator>

#include <ggmlutil/EngineFiles.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace {

    struct TemporaryDirectory {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("internal-tests-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        TemporaryDirectory() {
            std::filesystem::create_directories(path);
        }
        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
        std::filesystem::path write(const std::string &name, const std::string &text) {
            const auto result = path / name;
            std::ofstream stream(result, std::ios::binary);
            stream << text;
            BOOST_REQUIRE(stream.good());
            return result;
        }
    };

}

BOOST_AUTO_TEST_CASE(scratch_directories_are_distinct_and_float_wav_keeps_samples) {
    auto first = otter::ggml::makeScratchDirectory("otter-class-tests");
    auto second = otter::ggml::makeScratchDirectory("otter-class-tests");
    BOOST_REQUIRE(first);
    BOOST_REQUIRE(second);
    const auto firstPath = first.take();
    const auto secondPath = second.take();
    BOOST_CHECK(firstPath != secondPath);
    BOOST_CHECK(std::filesystem::is_directory(firstPath));
    const std::vector<float> samples{-0.5f, 0, 0.5f};
    const auto wave = firstPath / "audio.wav";
    auto written = otter::ggml::writeWav(wave, samples, 16000);
    otter::ggml::removeScratchDirectory(secondPath);
    BOOST_REQUIRE(written);
    std::ifstream stream(wave, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), {});
    BOOST_REQUIRE_EQUAL(bytes.size(), 44u + samples.size() * sizeof(float));
    BOOST_CHECK_EQUAL(std::string(bytes.data(), 4), "RIFF");
    BOOST_CHECK_EQUAL(std::string(bytes.data() + 8, 4), "WAVE");
    BOOST_CHECK_EQUAL(
        std::memcmp(bytes.data() + 44, samples.data(), samples.size() * sizeof(float)), 0);
    stream.close();
    otter::ggml::removeScratchDirectory(firstPath);
    BOOST_CHECK(!std::filesystem::exists(firstPath));
    BOOST_CHECK(!std::filesystem::exists(secondPath));
    otter::ggml::removeScratchDirectory(firstPath);
}

BOOST_AUTO_TEST_CASE(write_failures_are_reported) {
    TemporaryDirectory temporary;
    auto invalid = otter::ggml::writeWav(temporary.path / "absent" / "audio.wav", {1}, 16000);
    BOOST_REQUIRE(!invalid);
    BOOST_CHECK(invalid.error().code() == srt::Error::FileNotOpen);
}
