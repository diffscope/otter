// The game-ggml provider.
//
// The plumbing is verified in every run of a build that compiled the plugin, against a stand-in
// command line tool that answers the engine's protocol with canned output: the declaration surface,
// the refusals, the file checks of analyzer creation, and one whole execution through the spawn,
// the binary request frame and the JSON answer. What needs the real engine runs when the
// environment names its CLI and model, and reports itself as a skip otherwise, because a ggml
// release is an artifact no build should fetch on its own.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/SVS/InferenceContrib.h>
#include <synthrt/Support/Error.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Api/Note/1/NoteApiL1.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace NoteApi = otter::Api::Note::L1;
namespace CommonApi = otter::Api::Common::L1;

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
            root = std::filesystem::temp_directory_path() / "otter-game-ggml-test";
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root / "package" / "inferences" / "note");
            std::vector<std::filesystem::path> pluginPaths = {OTTER_TEST_PLUGIN_DIR};
            unit.setPluginPaths(srt::InferenceCategory::NAME, pluginPaths);
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
                      "    \"exports\": " + std::string(EXPORTS) + ",\n"
                      "    \"configuration\": " + wiring + "\n}");
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
                      "    \"exports\": " + exports + ",\n"
                      "    \"configuration\": " + wiring + "\n}");
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

        std::unique_ptr<NoteApi::NoteExecutive> open() {
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

BOOST_AUTO_TEST_SUITE(test_GameGgml)

BOOST_AUTO_TEST_CASE(test_GameGgml_ReportsWhatTheDeclarationExports) {
    Host host;
    host.write();
    auto spec = host.load();
    auto schema = spec->exports()->as<NoteApi::NoteSchema>();
    BOOST_REQUIRE(schema != nullptr);

    BOOST_CHECK_EQUAL(schema->sampleRate, RATE);
    BOOST_CHECK_EQUAL(schema->channelCount, 1);
    BOOST_CHECK_CLOSE(schema->maxSegmentDuration, 30.0, 1e-9);
    BOOST_CHECK(!schema->supportsKnownNotes);
    BOOST_REQUIRE_EQUAL(schema->languages.size(), 1u);
    BOOST_CHECK_EQUAL(schema->languages.front(), "cmn");
    BOOST_CHECK_EQUAL(schema->defaultLanguage, "cmn");
    BOOST_CHECK(schema->steps.honored);
    BOOST_CHECK_EQUAL(schema->steps.defaultValue, 8);
    BOOST_CHECK_CLOSE(schema->boundaryRadius.defaultValue, 0.02, 1e-9);
    BOOST_CHECK_CLOSE(schema->notePresenceCutoff.defaultValue, 0.5, 1e-9);
}

BOOST_AUTO_TEST_CASE(test_GameGgml_RefusesTheDeclarationsItCannotHonor) {
    // The serve protocol runs no alignment model, so a declaration that promises the alignment
    // path is refused at load, where the host can still pick another package.
    {
        Host host;
        host.writeWiring(R"({"cli": "x", "model": "y", "languages": {"cmn": 1}})",
                         R"({
                             "sampleRate": 44100,
                             "channelCount": 1,
                             "maxSegmentDuration": 30,
                             "languages": ["cmn"],
                             "defaultLanguage": "cmn",
                             "supportsKnownNotes": true
                         })");
        BOOST_CHECK(host.refusal().find("cannot be conditioned on known notes") !=
                    std::string::npos);
    }
    // A language the exports promise but the configuration does not number would leave the
    // execution without a number to pass to the model.
    {
        Host host;
        host.writeWiring(R"({"cli": "x", "model": "y", "languages": {"yue": 1}})");
        BOOST_CHECK(host.refusal().find("no numbering") != std::string::npos);
    }
    // A configuration key outside the variant's own vocabulary is a misspelling, and an accepted
    // misspelling would leave the intended value silently absent.
    {
        Host host;
        host.writeWiring(
            R"({"cli": "x", "model": "y", "languages": {"cmn": 1}, "scheduleStart": 0})");
        BOOST_CHECK(host.refusal().find("scheduleStart") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(test_GameGgml_AnalyzerCreationChecksTheFiles) {
    // A package whose engine is missing is one that cannot run, and the refusal names it.
    {
        Host host;
        host.write("cli/game_ggml_cli.exe");
        auto spec = host.load();
        auto made = NoteApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
        BOOST_REQUIRE(!made);
        BOOST_CHECK(made.error().rootCause().message().find("cannot find the game CLI") !=
                    std::string::npos);
    }
    {
        Host host;
        host.write(OTTER_TEST_FAKE_CLI, "models/absent.gguf");
        auto spec = host.load();
        auto made = NoteApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
        BOOST_REQUIRE(!made);
        BOOST_CHECK(made.error().rootCause().message().find("cannot find the game model") !=
                    std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(test_GameGgml_TranscribesAgainstTheFakeCli) {
    Host host;
    host.write();
    auto analyzer = host.open();

    NoteApi::NoteStartInput input;
    input.audio = tone(2.0, 7.5);
    std::vector<double> reported;
    input.progress = [&reported](double value) {
        reported.push_back(value);
        return true;
    };

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), why(produced));
    auto result = produced.take();

    // The fake engine answers with one voiced note of half a second and one rest. The default
    // cutoff is above the confidence of a rest, so the transcription keeps the first only.
    BOOST_REQUIRE_EQUAL(result->notes.size(), 1u);
    BOOST_CHECK_EQUAL(result->notes.front().key, 60);
    BOOST_CHECK_CLOSE(result->notes.front().start, 7.5, 1e-6);
    BOOST_CHECK_CLOSE(result->notes.front().duration, 0.5, 1e-6);
    BOOST_CHECK_CLOSE(result->notes.front().confidence, 1.0, 1e-9);

    // The engine call is monolithic, so the execution reports its progress at its start and its
    // end and nowhere between.
    BOOST_REQUIRE_GE(reported.size(), 2u);
    BOOST_CHECK_CLOSE(reported.front(), 0.0, 1e-9);
    BOOST_CHECK_CLOSE(reported.back(), 1.0, 1e-9);

    // A second execution reuses the same analyzer, whose engine process is still warm.
    NoteApi::NoteStartInput again;
    again.audio = tone(1.0, 1.0);
    auto second = analyzer->start(again);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(second), why(second));
    BOOST_REQUIRE_EQUAL(second.take()->notes.size(), 1u);
}

BOOST_AUTO_TEST_CASE(test_GameGgml_RefusesKnownNotes) {
    Host host;
    host.write();
    auto analyzer = host.open();

    NoteApi::NoteStartInput input;
    input.audio = tone(2.0);
    input.knownNotes = {{0.5, 0.5}};
    auto refused = analyzer->start(input);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().rootCause().message().find(
                    "cannot be conditioned on known notes") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_GameGgml_TranscribesAToneWithTheRealEngine) {
    Host host;
    const auto cli = pathFromEnv("OTTER_TEST_GAME_CLI", "no game.cpp CLI; set OTTER_TEST_GAME_CLI "
                                                    "to run the engine tests");
    const auto model = pathFromEnv("OTTER_TEST_GAME_GGUF", "no GAME GGUF model; set "
                                                     "OTTER_TEST_GAME_GGUF to run the engine tests");
    host.write(cli, model);
    auto spec = host.load();
    auto made = NoteApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), why(made));
    const auto analyzer = made.take();

    NoteApi::NoteStartInput input;
    input.audio = tone(2.0, 7.5);
    std::vector<double> reported;
    input.progress = [&reported](double value) {
        reported.push_back(value);
        return true;
    };

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), why(produced));
    auto result = produced.take();

    // The notes are ordered, non-overlapping, lie inside the span, and sit on the host's timeline
    // in seconds. What the engine makes of a constant tone is its own business; where it placed
    // them is the provider's promise.
    for (std::size_t i = 0; i < result->notes.size(); ++i) {
        const auto &note = result->notes[i];
        BOOST_CHECK_LE(note.start, 9.5);
        BOOST_CHECK_GE(note.start, 7.5);
        BOOST_CHECK_LE(note.start + note.duration, 9.5 + 1e-4);
        BOOST_CHECK_GT(note.duration, 0.0);
        // The default cutoff is above the confidence of a rest, so every note that survived is
        // one the engine called voiced.
        BOOST_CHECK_CLOSE(note.confidence, 1.0, 1e-9);
        if (i != 0) {
            BOOST_CHECK_GE(note.start, result->notes[i - 1].start +
                                            result->notes[i - 1].duration - 1e-4);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
