// The tifa-ggml provider.
//
// The shape mirrors its note sibling: the plumbing is verified in every run of a build that
// compiled the plugin, against a stand-in command line tool that writes a canned TextGrid, and
// what needs the real engine runs when the environment names its CLI and model, reporting itself
// as a skip otherwise.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/SVS/InferenceContrib.h>
#include <synthrt/Support/Error.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Api/Align/1/AlignApiL1.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace AlignApi = otter::Api::Align::L1;
namespace CommonApi = otter::Api::Common::L1;

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
            std::vector<std::filesystem::path> pluginPaths = {OTTER_TEST_PLUGIN_DIR};
            unit.setPluginPaths(srt::InferenceCategory::NAME, pluginPaths);
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
                   const std::string &dictPath = "models",
                   const std::string &exports = EXPORTS,
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
                      "    \"exports\": " + exports + ",\n"
                      "    \"configuration\": " + wiring + "\n}");
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

BOOST_AUTO_TEST_SUITE(test_TifaGgml)

BOOST_AUTO_TEST_CASE(test_TifaGgml_ReportsWhatTheDeclarationExports) {
    Host host;
    host.write();
    auto spec = host.load();
    auto schema = spec->exports()->as<AlignApi::AlignSchema>();
    BOOST_REQUIRE(schema != nullptr);

    BOOST_CHECK_EQUAL(schema->sampleRate, RATE);
    BOOST_CHECK_EQUAL(schema->channelCount, 1);
    BOOST_CHECK_CLOSE(schema->maxSegmentDuration, 300.0, 1e-9);
    BOOST_CHECK_EQUAL(schema->silenceLabel, "SP");
    BOOST_REQUIRE_EQUAL(schema->languages.size(), 1u);
    BOOST_CHECK_EQUAL(schema->languages.front().language, "cmn");
    BOOST_CHECK_EQUAL(schema->languages.front().scheme, "pinyin");
    BOOST_CHECK(schema->languages.front().lyrics == AlignApi::LyricsForm::Scheme);
    // The variant reads no knobs: the engine takes no alignment knobs the contract could carry.
    BOOST_CHECK(!schema->nonSpeechThreshold.honored);
    BOOST_CHECK(!schema->nonSpeechMinDuration.honored);
    BOOST_CHECK(!schema->gapFill.honored);
}

BOOST_AUTO_TEST_CASE(test_TifaGgml_RefusesTheDeclarationsItCannotHonor) {
    // The engine detects no non-speech sound, so promising one would make "none found" and
    // "nothing was looked for" the same answer.
    {
        Host host;
        host.write(OTTER_TEST_FAKE_CLI, "models/tifa.gguf", "path/to/dicts",
                   R"({
                       "sampleRate": 48000,
                       "channelCount": 1,
                       "maxSegmentDuration": 300,
                       "languages": [
                           {"language": "cmn", "scheme": "pinyin", "lyrics": "scheme",
                            "phonemes": ["a", "b"]}
                       ],
                       "defaultLanguage": "cmn",
                       "silenceLabel": "SP",
                       "nonSpeechPhonemes": ["AP"],
                       "defaultNonSpeechPhonemes": ["AP"]
                   })",
                   R"({"cli": "x", "model": "y", "languages": {"cmn": "zh"}})");
        BOOST_CHECK(host.refusal().find("detects no non-speech sound") != std::string::npos);
    }
    // A language the exports list without an engine code leaves the G2P without a language.
    {
        Host host;
        host.write(OTTER_TEST_FAKE_CLI, "models/tifa.gguf", "path/to/dicts", EXPORTS,
                   R"({"cli": "x", "model": "y", "languages": {}})");
        BOOST_CHECK(host.refusal().find("no engine code") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(test_TifaGgml_AnalyzerCreationChecksTheFiles) {
    // A package whose engine is missing is one that cannot run, and the refusal names it.
    {
        Host host;
        host.write("cli/tifa_ggml_cli.exe");
        auto spec = host.load();
        auto made = AlignApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
        BOOST_REQUIRE(!made);
        BOOST_CHECK(made.error().rootCause().message().find("cannot find the tifa CLI") !=
                    std::string::npos);
    }
    {
        Host host;
        host.write(OTTER_TEST_FAKE_CLI, "models/absent.gguf");
        auto spec = host.load();
        auto made = AlignApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
        BOOST_REQUIRE(!made);
        BOOST_CHECK(made.error().rootCause().message().find("cannot find the tifa model") !=
                    std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(test_TifaGgml_AlignsAgainstTheFakeCli) {
    Host host;
    host.write();
    auto analyzer = host.open();

    // The fake tool aligns a leading gap and two words over one second: "ni" from 0.2 to 0.6 and
    // "hao" from 0.6 to 1, each with two phones. Against a one-second span the leading gap is the
    // only silence, and against a two-second span the second half is another.
    AlignApi::AlignStartInput input;
    input.audio = tone(1.0, 3.0);
    input.lyrics = "ni hao";
    std::vector<double> reported;
    input.progress = [&reported](double value) {
        reported.push_back(value);
        return true;
    };

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), why(produced));
    auto result = produced.take();

    BOOST_CHECK_EQUAL(result->language, "cmn");
    BOOST_CHECK_EQUAL(result->scheme, "pinyin");
    BOOST_REQUIRE_EQUAL(result->words.size(), 3u);

    // The leading gap is a word of the silence label, and the words after it begin where their
    // own phones begin, not where the span does.
    BOOST_CHECK_EQUAL(result->words.front().text, "SP");
    BOOST_CHECK_CLOSE(result->words.front().start, 3.0, 1e-6);
    BOOST_CHECK_CLOSE(result->words.front().duration, 0.2, 1e-3);
    BOOST_REQUIRE_EQUAL(result->words.front().phones.size(), 1u);
    BOOST_CHECK_EQUAL(result->words[1].text, "ni");
    BOOST_CHECK_CLOSE(result->words[1].start, 3.2, 1e-3);
    BOOST_CHECK_CLOSE(result->words[1].duration, 0.4, 1e-3);
    BOOST_REQUIRE_EQUAL(result->words[1].phones.size(), 2u);
    BOOST_CHECK_EQUAL(result->words[1].phones.front().text, "n");
    BOOST_CHECK_EQUAL(result->words.back().text, "hao");
    BOOST_CHECK_CLOSE(result->words.back().start, 3.6, 1e-3);
    BOOST_REQUIRE_EQUAL(result->words.back().phones.size(), 2u);
    BOOST_CHECK_EQUAL(result->words.back().phones.front().text, "x");

    // The phones cover each word exactly, and the words cover the span.
    for (std::size_t i = 0; i < result->words.size(); ++i) {
        const auto &word = result->words[i];
        double covered = 0;
        for (const auto &phone : word.phones) {
            BOOST_CHECK_GT(phone.duration, 0.0);
            covered += phone.duration;
        }
        BOOST_CHECK_LE(std::abs(covered - word.duration), 1e-4);
        if (i != 0) {
            BOOST_CHECK_CLOSE(word.start,
                              result->words[i - 1].start + result->words[i - 1].duration, 1e-3);
        }
    }
    BOOST_CHECK_CLOSE(result->words.back().start + result->words.back().duration, 4.0, 1e-3);

    BOOST_REQUIRE_GE(reported.size(), 2u);
    BOOST_CHECK_CLOSE(reported.front(), 0.0, 1e-9);
    BOOST_CHECK_CLOSE(reported.back(), 1.0, 1e-9);

    // The stretch the decode attributed to no word becomes a word of the silence label, so the
    // result of a longer span still covers it.
    AlignApi::AlignStartInput over;
    over.audio = tone(2.0);
    over.lyrics = "ni hao";
    auto padded = analyzer->start(over);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(padded), why(padded));
    auto paddedResult = padded.take();
    BOOST_REQUIRE_GE(paddedResult->words.size(), 4u);
    BOOST_CHECK_EQUAL(paddedResult->words.back().text, "SP");
    BOOST_CHECK_CLOSE(paddedResult->words.back().start, 1.0, 1e-3);
    BOOST_CHECK_CLOSE(paddedResult->words.back().start + paddedResult->words.back().duration, 2.0,
                      1e-3);
}

BOOST_AUTO_TEST_CASE(test_TifaGgml_AlignsLyricsWithTheRealEngine) {
    Host host;
    const auto cli = pathFromEnv("OTTER_TEST_TIFA_CLI", "no tifa.cpp CLI; set OTTER_TEST_TIFA_CLI "
                                                        "to run the engine tests");
    const auto model = pathFromEnv("OTTER_TEST_TIFA_GGUF", "no TIFA GGUF model; set "
                                                     "OTTER_TEST_TIFA_GGUF to run the engine tests");
    const auto dicts = pathFromEnv("OTTER_TEST_TIFA_DICTS", "no TIFA G2P dictionaries; set "
                                    "OTTER_TEST_TIFA_DICTS to run the engine tests");
    host.write(cli, model, dicts);
    auto spec = host.load();
    auto made = AlignApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), why(made));
    const auto analyzer = made.take();

    AlignApi::AlignStartInput input;
    input.audio = tone(1.0, 3.0);
    input.lyrics = "ni hao";
    std::vector<double> reported;
    input.progress = [&reported](double value) {
        reported.push_back(value);
        return true;
    };

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), why(produced));
    auto result = produced.take();

    BOOST_CHECK_EQUAL(result->language, "cmn");
    BOOST_CHECK_EQUAL(result->scheme, "pinyin");
    BOOST_REQUIRE_GE(result->words.size(), 1u);

    // The words cover the span: the declaration carries a silence label, so the gaps the decode
    // attributed to no word are words of their own, and the result runs from the start of the
    // audio to its end without a hole.
    BOOST_CHECK_CLOSE(result->words.front().start, 3.0, 1e-6);
    for (std::size_t i = 0; i < result->words.size(); ++i) {
        const auto &word = result->words[i];
        BOOST_CHECK_GT(word.duration, 0.0);
        BOOST_REQUIRE_GE(word.phones.size(), 1u);
        double covered = 0;
        for (const auto &phone : word.phones) {
            BOOST_CHECK_GT(phone.duration, 0.0);
            covered += phone.duration;
        }
        BOOST_CHECK_LE(std::abs(covered - word.duration), 1e-4);
        if (i != 0) {
            BOOST_CHECK_CLOSE(word.start,
                              result->words[i - 1].start + result->words[i - 1].duration, 1e-3);
        }
    }
    BOOST_CHECK_CLOSE(result->words.back().start + result->words.back().duration, 4.0, 1e-3);

    BOOST_REQUIRE_GE(reported.size(), 2u);
    BOOST_CHECK_CLOSE(reported.front(), 0.0, 1e-9);
    BOOST_CHECK_CLOSE(reported.back(), 1.0, 1e-9);
}

BOOST_AUTO_TEST_SUITE_END()
