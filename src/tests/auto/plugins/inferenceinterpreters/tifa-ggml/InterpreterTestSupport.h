#ifndef OTTER_TESTS_PLUGINS_INFERENCEINTERPRETERS_TIFA_GGML_INTERPRETERTESTSUPPORT_H
#define OTTER_TESTS_PLUGINS_INFERENCEINTERPRETERS_TIFA_GGML_INTERPRETERTESTSUPPORT_H

#include <system_error>

// The tifa-ggml provider.
//
// The shape mirrors its note sibling: the plumbing is verified in every run of a build that
// compiled the plugin, against a stand-in command line tool that writes a canned TextGrid, and
// what needs the real engine runs when the environment names its CLI and model, reporting itself
// as a skip otherwise.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/SVS/InferenceContrib.h>
#include <synthrt/Support/Error.h>
#include <synthrt/SVS/InferenceInterpreterPlugin.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Api/Align/1/AlignApiL1.h>

#include <tifa-ggml/TifaGgmlInterpreter.h>

#include <boost/test/unit_test.hpp>

#include "TestSupport.h"

namespace AlignApi = otter::Api::Align::L1;
namespace CommonApi = otter::Api::Common::L1;

namespace {

    /// The plugin that main.cpp of tifa-ggml declares, around the interpreter that this test
    /// links. The test registers it as a static plugin with the plugin.json of tifa-ggml, so that
    /// discovery selects this class rather than the library in the plugin tree.
    class TifaGgmlPlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(const srt::ContribTriplet &triplet) override {
            if (triplet != srt::ContribTriplet(AlignApi::API_INTERFACE, AlignApi::API_LEVEL,
                                               otter::tifaGgml::VARIANT)) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this plugin serves the " + std::string(AlignApi::API_INTERFACE) +
                                      " contract at level " + std::to_string(AlignApi::API_LEVEL) +
                                      " only, in the variant " + otter::tifaGgml::VARIANT);
            }
            return std::unique_ptr<srt::ContribInterpreter>(
                new otter::tifaGgml::TifaGgmlInterpreter());
        }
    };

}

STDC_EXPORT_STATIC_PLUGIN(TifaGgmlPlugin, srt::InferenceInterpreterPlugin::IID,
                          otter::test::pluginManifest())

namespace {

    constexpr int RATE = 48000;

    /// The exports of the test package: one language, whose phonemes the real-engine tests replace
    /// with the ones the loaded vocabulary spells, and a silence label the gaps take.
    constexpr char EXPORTS[] = R"({
        "sampleRate": 48000,
        "channelCount": 1,
        "maxSegmentDuration": 300,
        "languages": [
            {"language": "cmn", "scheme": "pinyin", "lyrics": "scheme", "phonemes": ["a", "b"]}
        ],
        "defaultLanguage": "cmn",
        "silenceLabel": "SP"
    })";

    /// The configuration of the test package. The engine, the model and the dictionaries take the
    /// places of the placeholders, so a test that names the real engine keeps the rest.
    constexpr char CONFIGURATION[] = R"({
        "cli": "cli/tifa_ggml_cli.exe",
        "model": "models/tifa.gguf",
        "dictionaries": "models",
        "languages": {"cmn": "zh"}
    })";

    template <class Expected>
    std::string why(const Expected &expected) {
        return expected ? std::string() : expected.error().toString();
    }

    void skip(const char *what) {
        std::cerr << "SKIP: " << what << "\n";
        std::exit(OTTER_TEST_SKIP_EXIT_CODE);
    }

    std::string pathFromEnv(const char *name, const char *what) {
        const char *given = std::getenv(name);
        if (given == nullptr || *given == '\0') {
            skip(what);
        }
        std::string result(given);
        std::replace(result.begin(), result.end(), '\\', '/');
        return result;
    }

    /// A host that writes one package into a scratch directory and loads it through a unit that
    /// discovers the ggml interpreters from the build's plugin tree.
    struct Host {
        srt::SynthUnit unit;
        srt::PackageHandle package;
        std::filesystem::path root;

        Host() {
            root = std::filesystem::temp_directory_path() / "otter-tifa-ggml-test";
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root / "package" / "inferences" / "align");
        }

        ~Host() {
            package.reset();
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }

        /// Writes the declaration of a package whose engine, model and dictionaries live at the
        /// given paths. The model file is created as a small file, because analyzer creation checks
        /// that it exists and nothing else.
        void write(const std::string &cliPath = OTTER_TEST_FAKE_CLI,
                   const std::string &modelPath = "models/tifa.gguf",
                   const std::string &dictPath = "models", const std::string &exports = EXPORTS,
                   const std::string &configuration = CONFIGURATION) {
            std::string wiring = configuration;
            if (const auto at = wiring.find("cli/tifa_ggml_cli.exe"); at != std::string::npos) {
                wiring.replace(at, std::strlen("cli/tifa_ggml_cli.exe"), cliPath);
            }
            if (const auto at = wiring.find("models/tifa.gguf"); at != std::string::npos) {
                wiring.replace(at, std::strlen("models/tifa.gguf"), modelPath);
            }
            if (const auto at = wiring.find("\"models\""); at != std::string::npos) {
                wiring.replace(at, std::strlen("\"models\""), "\"" + dictPath + "\"");
            }
            // The stand-in model must exist where the declaration's relative path resolves: the
            // directory of the declaration file, as spec 2.4 defines it, not the package root.
            writeFile(root / "package" / "inferences" / "align" / "models" / "tifa.gguf",
                      "a stand-in model");
            writeFile(root / "package" / "desc.json", R"({
                "$version": "1.0",
                "id": "otter/test-tifa-ggml",
                "version": "1.0.0.0",
                "runtimeLevel": 1,
                "contributions": {
                    "inference": [ { "id": "align", "path": "./inferences/align/inference.json" } ]
                }
            })");
            writeFile(root / "package" / "inferences" / "align" / "inference.json",
                      "{\n"
                      "    \"interface\": \"org.openvpi.otter.inference.Align\",\n"
                      "    \"level\": 1,\n"
                      "    \"variant\": \"tifa-ggml\",\n"
                      "    \"name\": \"GGML TIFA\",\n"
                      "    \"exports\": " +
                          exports +
                          ",\n"
                          "    \"configuration\": " +
                          wiring + "\n}");
        }

        srt::ContribSpec *load() {
            auto opened = unit.openPackage(root / "package", srt::SynthUnit::Load);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), why(opened));
            package = opened.take();
            auto spec = package.contribution(srt::InferenceCategory::NAME, "align");
            BOOST_REQUIRE(spec != nullptr);
            return spec;
        }

        std::string refusal() {
            auto opened = unit.openPackage(root / "package", srt::SynthUnit::Load);
            BOOST_REQUIRE(!opened);
            return opened.error().rootCause().message();
        }

        std::unique_ptr<AlignApi::AlignExecutive> open() {
            auto spec = load();
            auto made = AlignApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), why(made));
            return made.take();
        }

    private:
        static void writeFile(const std::filesystem::path &path, const std::string &text) {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(out), "cannot write " + path.string());
            out << text;
        }
    };

    CommonApi::AudioSegment tone(double seconds, double startTime = 0) {
        CommonApi::AudioSegment audio;
        audio.sampleRate = RATE;
        audio.channelCount = 1;
        audio.samples.assign(static_cast<std::size_t>(seconds * RATE), 0.25f);
        audio.startTime = startTime;
        return audio;
    }

}


#endif // OTTER_TESTS_PLUGINS_INFERENCEINTERPRETERS_TIFA_GGML_INTERPRETERTESTSUPPORT_H
