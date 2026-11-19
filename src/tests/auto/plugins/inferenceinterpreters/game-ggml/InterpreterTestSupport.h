#ifndef OTTER_TESTS_PLUGINS_INFERENCEINTERPRETERS_GAME_GGML_INTERPRETERTESTSUPPORT_H
#define OTTER_TESTS_PLUGINS_INFERENCEINTERPRETERS_GAME_GGML_INTERPRETERTESTSUPPORT_H

#include <system_error>

// The game-ggml provider.
//
// The plumbing is verified in every run of a build that compiled the plugin, against a stand-in
// command line tool that answers the engine's protocol with canned output: the declaration surface,
// the refusals, the file checks of analyzer creation, and one whole execution through the spawn,
// the binary request frame and the JSON answer. What needs the real engine runs when the
// environment names its CLI and model, and reports itself as a skip otherwise, because a ggml
// release is an artifact no build should fetch on its own.

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
#include <random>
#include <string>
#include <vector>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/SVS/InferenceContrib.h>
#include <synthrt/Support/Error.h>
#include <synthrt/SVS/InferenceInterpreterPlugin.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Api/Note/1/NoteApiL1.h>

#include <game-ggml/GameGgmlInterpreter.h>

#include <boost/test/unit_test.hpp>

#include "TestSupport.h"

namespace NoteApi = otter::Api::Note::L1;
namespace CommonApi = otter::Api::Common::L1;

namespace {

    /// The plugin that main.cpp of game-ggml declares, around the interpreter that this test
    /// links. The test registers it as a static plugin with the plugin.json of game-ggml, so that
    /// discovery selects this class rather than the library in the plugin tree.
    class GameGgmlPlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(const srt::ContribContractTriplet &triplet) override {
            if (triplet != srt::ContribContractTriplet(NoteApi::Contract.interfaceId(),
                                                       NoteApi::Contract.level(),
                                                       otter::gameGgml::Triplet.variant())) {
                return srt::Error(
                    srt::Error::FeatureNotSupported,
                    "this plugin serves the " + std::string(NoteApi::Contract.interfaceId()) +
                        " contract at level " + std::to_string(NoteApi::Contract.level()) +
                        " only, in the variant " + otter::gameGgml::Triplet.variant());
            }
            return std::unique_ptr<srt::ContribInterpreter>(
                new otter::gameGgml::GameGgmlInterpreter());
        }
    };

}

STDC_EXPORT_STATIC_PLUGIN(GameGgmlPlugin, srt::InferenceInterpreterPlugin::IID,
                          otter::test::pluginManifest())

namespace {

    constexpr int RATE = 44100;

    /// The exports of the test package: one language, every knob honored, no alignment path.
    constexpr char EXPORTS[] = R"({
        "sampleRate": 44100,
        "channelCount": 1,
        "maxSegmentDuration": 30,
        "languages": ["cmn"],
        "defaultLanguage": "cmn",
        "knobs": {
            "steps": {"minimum": 1, "maximum": 16, "default": 8},
            "boundaryThreshold": {"minimum": 0.0, "maximum": 1.0, "default": 0.2},
            "boundaryRadius": {"minimum": 0.0, "maximum": 1.0, "default": 0.02},
            "noteThreshold": {"minimum": 0.0, "maximum": 1.0, "default": 0.2},
            "notePresenceCutoff": {"minimum": 0.0, "maximum": 1.0, "default": 0.5}
        }
    })";

    /// The configuration of the test package, numbering cmn as the model's language one.
    constexpr char CONFIGURATION[] = R"({
        "cli": "cli/game_ggml_cli.exe",
        "model": "models/game.gguf",
        "languages": {"cmn": 1},
        "timestep": 0.01
    })";

    template <class Expected>
    std::string why(const Expected &expected) {
        return expected ? std::string() : expected.error().toString();
    }

    /// Ends the test run with the status that ctest reports as a skip.
    void skip(const char *what) {
        std::cerr << "SKIP: " << what << "\n";
        std::exit(OTTER_TEST_SKIP_EXIT_CODE);
    }

    /// Returns the path the environment names, as a JSON string with forward slashes.
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
            const auto suffix =
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(std::random_device{}());
            root = std::filesystem::temp_directory_path() / ("otter-game-ggml-test-" + suffix);
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root / "package" / "inferences" / "note");
        }

        ~Host() {
            package.reset();
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }

        /// Writes the declaration of a package whose engine and model live at the given paths,
        /// over the placeholders of the configuration. The model file is created as an empty file,
        /// because analyzer creation checks that it exists and nothing else.
        void write(const std::string &cliPath = OTTER_TEST_FAKE_CLI,
                   const std::string &modelPath = "models/game.gguf",
                   const std::string &configuration = CONFIGURATION) {
            std::string wiring = configuration;
            if (const auto at = wiring.find("cli/game_ggml_cli.exe"); at != std::string::npos) {
                wiring.replace(at, std::strlen("cli/game_ggml_cli.exe"), cliPath);
            }
            if (const auto at = wiring.find("models/game.gguf"); at != std::string::npos) {
                wiring.replace(at, std::strlen("models/game.gguf"), modelPath);
            }
            // The stand-in model must exist where the declaration's relative path resolves: the
            // directory of the declaration file, as spec 2.4 defines it, not the package root.
            writeFile(root / "package" / "inferences" / "note" / "models" / "game.gguf",
                      "a stand-in model");
            writeFile(root / "package" / "desc.json", R"({
                "$version": "1.0",
                "id": "otter/test-game-ggml",
                "version": "1.0.0.0",
                "runtimeLevel": 1,
                "contributions": {
                    "inference": [ { "id": "note", "path": "./inferences/note/inference.json" } ]
                }
            })");
            writeFile(root / "package" / "inferences" / "note" / "inference.json",
                      "{\n"
                      "    \"interface\": \"org.openvpi.otter.inference.Note\",\n"
                      "    \"level\": 1,\n"
                      "    \"variant\": \"game-ggml\",\n"
                      "    \"name\": \"GGML GAME\",\n"
                      "    \"exports\": " +
                          std::string(EXPORTS) +
                          ",\n"
                          "    \"configuration\": " +
                          wiring + "\n}");
        }

        /// Writes the declaration so that its configuration names \a wiring verbatim and its
        /// exports state \a exports.
        void writeWiring(const std::string &wiring, const std::string &exports = EXPORTS) {
            writeFile(root / "package" / "desc.json", R"({
                "$version": "1.0",
                "id": "otter/test-game-ggml",
                "version": "1.0.0.0",
                "runtimeLevel": 1,
                "contributions": {
                    "inference": [ { "id": "note", "path": "./inferences/note/inference.json" } ]
                }
            })");
            writeFile(root / "package" / "inferences" / "note" / "inference.json",
                      "{\n"
                      "    \"interface\": \"org.openvpi.otter.inference.Note\",\n"
                      "    \"level\": 1,\n"
                      "    \"variant\": \"game-ggml\",\n"
                      "    \"name\": \"GGML GAME\",\n"
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
            auto spec = package.contribution(srt::InferenceCategory::NAME, "note");
            BOOST_REQUIRE(spec != nullptr);
            return spec;
        }

        /// Returns the text of the root cause of the load failure, which the test expects.
        std::string refusal() {
            auto opened = unit.openPackage(root / "package", srt::SynthUnit::Load);
            BOOST_REQUIRE(!opened);
            return opened.error().rootCause().message();
        }

        std::unique_ptr<NoteApi::Executive> open() {
            auto spec = load();
            auto made = NoteApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
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


#endif // OTTER_TESTS_PLUGINS_INFERENCEINTERPRETERS_GAME_GGML_INTERPRETERTESTSUPPORT_H
