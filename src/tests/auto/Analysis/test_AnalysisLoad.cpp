// Loading an analysis package: the declarations that the category accepts, the values that the
// provider reads from a declaration, and the information available to a host before it creates an
// analyzer.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/SVS/InferenceContrib.h>

#include <otter/Analysis/AnalysisExecutive.h>
#include <otter/Api/Align/1/AlignApiL1.h>
#include <otter/Api/F0/1/F0ApiL1.h>
#include <otter/Api/Note/1/NoteApiL1.h>

#include "TestSupport.h"

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace {

    namespace fs = std::filesystem;
    namespace AlignApi = otter::Api::Align::L1;
    namespace F0Api = otter::Api::F0::L1;
    namespace NoteApi = otter::Api::Note::L1;

    fs::path packages() {
        return fs::path(OTTER_TEST_PACKAGE_DIR);
    }

    /// A unit configured as a host configures it: the stub provider on the plugin path of the
    /// inference category.
    struct Unit {
        Unit() {
            const fs::path paths[] = {fs::path(OTTER_TEST_PLUGIN_DIR)};
            unit.setPluginPaths(srt::InferenceCategory::NAME, paths);
        }

        srt::SynthUnit unit;
    };

    /// The languages of a stub aligner that loads, as a JSON member, without its default.
    const char ALIGN_LANGUAGES[] =
        R"("languages": [{"language": "cmn", "scheme": "pinyin", "lyrics": "scheme",)"
        R"( "phonemes": ["a"]}])";

    /// Writes a stub aligner package whose exports are \a exports, the sample rate aside, into a
    /// directory of its own under the system's temporary directory, and returns the package.
    fs::path writeAligner(const std::string &name, const std::string &exports) {
        const auto root = fs::temp_directory_path() / "otter-test-aligners" / name;
        fs::remove_all(root);
        fs::create_directories(root / "inferences" / "align");
        std::ofstream(root / "desc.json")
            << R"({"$version": "1.0", "id": "otter/test-)" << name
            << R"(", "version": "1.0.0.0", "runtimeLevel": 1, "contributions": {"inference": )"
            << R"([{"id": "align", "path": "./inferences/align/inference.json"}]}})";
        std::ofstream(root / "inferences" / "align" / "inference.json")
            << R"({"interface": "org.openvpi.otter.inference.Align", "level": 1, "variant": )"
            << R"("stub", "name": "Aligner", "exports": {"sampleRate": 16000)"
            << (exports.empty() ? "" : ", ") << exports << "}}";
        return root;
    }
}

BOOST_AUTO_TEST_SUITE(test_AnalysisLoad)

BOOST_AUTO_TEST_CASE(test_AnalysisLoad_ReadsBothContracts) {
    Unit host;
    auto opened = host.unit.openPackage(packages() / "stub-analyzers", srt::SynthUnit::Load);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), otter::test::why(opened));
    auto package = opened.take();

    auto category =
        host.unit.category(srt::InferenceCategory::NAME)->as<srt::InferenceCategory>();
    BOOST_REQUIRE_EQUAL(category->inferences().size(), 2u);

    auto f0 = package.contribution(srt::InferenceCategory::NAME, "f0");
    BOOST_REQUIRE(f0 != nullptr);
    BOOST_CHECK_EQUAL(f0->interface(), F0Api::API_INTERFACE);
    BOOST_CHECK_EQUAL(f0->level(), F0Api::API_LEVEL);
    BOOST_CHECK_EQUAL(f0->variant(), "stub");
    BOOST_CHECK_EQUAL(f0->name().text(), "Stub F0");

    // The values that the host reads before it creates an analyzer: the audio format to produce,
    // and the knobs that this module honors.
    auto schema = f0->exports()->as<F0Api::F0Schema>();
    BOOST_REQUIRE(schema != nullptr);
    BOOST_CHECK_EQUAL(schema->sampleRate, 16000);
    BOOST_CHECK_EQUAL(schema->channelCount, 1);
    BOOST_CHECK_CLOSE(schema->interval, 0.01, 1e-9);
    BOOST_CHECK_CLOSE(schema->maxSegmentDuration, 60.0, 1e-9);
    BOOST_CHECK(schema->voicingThreshold.honored);
    BOOST_CHECK_CLOSE(schema->voicingThreshold.defaultValue, 0.03, 1e-9);
    BOOST_CHECK(schema->interpolateUnvoiced.honored);

    auto note = package.contribution(srt::InferenceCategory::NAME, "note");
    BOOST_REQUIRE(note != nullptr);
    BOOST_CHECK_EQUAL(note->interface(), NoteApi::API_INTERFACE);
    auto noteSchema = note->exports()->as<NoteApi::NoteSchema>();
    BOOST_REQUIRE(noteSchema != nullptr);
    BOOST_CHECK(noteSchema->supportsKnownNotes);
    BOOST_REQUIRE_EQUAL(noteSchema->languages.size(), 1u);
    BOOST_CHECK_EQUAL(noteSchema->languages.front(), "zxx");
    BOOST_CHECK(noteSchema->steps.honored);
    BOOST_CHECK_EQUAL(noteSchema->steps.defaultValue, 8);

    // Each module implements its own contract only. The options used by a contract to create an
    // analyzer identify that contract, and the framework rejects them for a module of another
    // contract.
    auto f0Analyzer = F0Api::createAnalyzer(*f0->as<srt::InferenceSpec>());
    BOOST_CHECK_MESSAGE(static_cast<bool>(f0Analyzer), otter::test::why(f0Analyzer));
    BOOST_CHECK(!NoteApi::createAnalyzer(*f0->as<srt::InferenceSpec>()));
    auto noteAnalyzer = NoteApi::createAnalyzer(*note->as<srt::InferenceSpec>());
    BOOST_CHECK_MESSAGE(static_cast<bool>(noteAnalyzer), otter::test::why(noteAnalyzer));
}

BOOST_AUTO_TEST_CASE(test_AnalysisLoad_DataOnlyStopsBeforeTheProvider) {
    // A host that lists installed analyzers in a settings page requires the name, the contract
    // and the variant, and none of these requires an interpreter. Reading the audio format of the
    // model requires the interpreter.
    Unit host;
    auto opened = host.unit.openPackage(packages() / "stub-analyzers", srt::SynthUnit::DataOnly);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), otter::test::why(opened));
    auto package = opened.take();

    auto f0 = package.contribution(srt::InferenceCategory::NAME, "f0");
    BOOST_REQUIRE(f0 != nullptr);
    BOOST_CHECK_EQUAL(f0->interface(), F0Api::API_INTERFACE);
    BOOST_CHECK_EQUAL(f0->variant(), "stub");
    BOOST_CHECK(f0->exports() == nullptr);
    // The raw exports block of the declaration remains available to a host that reads it without
    // a provider. This availability is the reason for declaring the audio format in exports rather
    // than in the configuration.
    const auto &declared = f0->manifestExports();
    BOOST_REQUIRE(declared.isObject());
    BOOST_CHECK_EQUAL(declared.toObject().at("sampleRate").toInt(), 16000);
}

/// One reader reads the exports block for every variant, so every variant rejects the same
/// declarations: a missing audio format, a key the contract does not define, and a knob whose
/// default lies outside its range.
BOOST_AUTO_TEST_CASE(test_AnalysisLoad_RefusesExportsWithoutTheAudioFormat) {
    Unit host;
    auto opened =
        host.unit.openPackage(packages() / "bad-exports-missing-rate", srt::SynthUnit::Load);
    BOOST_REQUIRE(!opened);
    BOOST_CHECK(opened.error().rootCause().message().find("sampleRate") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_AnalysisLoad_RefusesAnUnknownExportsKey) {
    Unit host;
    auto opened =
        host.unit.openPackage(packages() / "bad-exports-unknown-key", srt::SynthUnit::Load);
    BOOST_REQUIRE(!opened);
    BOOST_CHECK(opened.error().rootCause().message().find("frameRate") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_AnalysisLoad_RefusesAKnobWhoseDefaultLeavesItsRange) {
    Unit host;
    auto opened =
        host.unit.openPackage(packages() / "bad-exports-knob-range", srt::SynthUnit::Load);
    BOOST_REQUIRE(!opened);
    BOOST_CHECK(opened.error().rootCause().message().find("voicingThreshold") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_AnalysisLoad_RejectsAnUnknownConfigurationKey) {
    // A misspelt key leaves the intended value silently absent, and for a threshold the result is
    // a working model that produces wrong results.
    Unit host;
    auto opened = host.unit.openPackage(packages() / "bad-configuration", srt::SynthUnit::Load);
    BOOST_REQUIRE(!opened);
    BOOST_CHECK(opened.error().rootCause().message().find("frequencey") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_AnalysisLoad_LetsAnotherModuleReferenceAnAnalyzer) {
    // The upper specification permits a category without an Executive Factory, and the Loader
    // accepts a null factory, so a package that references an analyzer is valid and must load.
    // Rejecting the import, as an earlier version of this category did, turns a valid third-party
    // package into a load failure.
    Unit host;
    auto opened = host.unit.openPackage(packages() / "importing", srt::SynthUnit::Load);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), otter::test::why(opened));
    auto package = opened.take();
    auto note = package.contribution(srt::InferenceCategory::NAME, "note");
    BOOST_REQUIRE(note != nullptr);
    BOOST_REQUIRE_EQUAL(note->imports().size(), 1u);
    BOOST_CHECK(note->findImport("analysis/reference").has_value());
    package.reset();
}

BOOST_AUTO_TEST_CASE(test_AnalysisLoad_RejectsImportOptionsNothingWillRead) {
    // Absent options are accepted. Written options express an intent that the author expects to
    // take effect, and no contract in this category defines import options.
    Unit host;
    auto opened =
        host.unit.openPackage(packages() / "importing-with-options", srt::SynthUnit::Load);
    BOOST_REQUIRE(!opened);
    BOOST_CHECK(opened.error().rootCause().message().find("no import options") !=
                std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_AnalysisLoad_RejectsAnEntryWithoutADeclaration) {
    Unit host;
    auto opened = host.unit.openPackage(packages() / "no-declaration", srt::SynthUnit::Load);
    BOOST_CHECK(!opened);
}

/// The Align reader belongs to the library and requires no model, so the stub is sufficient to
/// test it: each language declares the scheme of its phonemes, the form of its lyrics and the
/// phonemes that a result can contain.
BOOST_AUTO_TEST_CASE(test_AnalysisLoad_ReadsTheAlignContract) {
    Unit host;
    auto opened = host.unit.openPackage(packages() / "stub-aligner", srt::SynthUnit::Load);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), otter::test::why(opened));
    auto package = opened.take();
    auto align = package.contribution(srt::InferenceCategory::NAME, "align");
    BOOST_REQUIRE(align != nullptr);
    auto schema = align->exports()->as<AlignApi::AlignSchema>();
    BOOST_REQUIRE(schema != nullptr);
    BOOST_CHECK_EQUAL(schema->sampleRate, 16000);
    BOOST_REQUIRE_EQUAL(schema->languages.size(), 3u);
    BOOST_CHECK_EQUAL(schema->languages[0].language, "cmn");
    BOOST_CHECK_EQUAL(schema->languages[0].scheme, "pinyin");
    BOOST_CHECK(schema->languages[0].lyrics == AlignApi::LyricsForm::Scheme);
    BOOST_CHECK(schema->languages[0].phonemes == std::vector<std::string>({"a", "b"}));
    BOOST_CHECK(schema->languages[2].lyrics == AlignApi::LyricsForm::Text);
    BOOST_CHECK_EQUAL(schema->defaultLanguage, "cmn");
    BOOST_CHECK(schema->defaultNonSpeechPhonemes == std::vector<std::string>({"AP"}));
    BOOST_CHECK_EQUAL(schema->silenceLabel, "SP");
    BOOST_CHECK(schema->gapFill.honored);
    BOOST_CHECK(static_cast<bool>(AlignApi::createAnalyzer(*align->as<srt::InferenceSpec>())));
    package.reset();
}

/// Each of these exports blocks violates one rule of the Align reader, through which every
/// variant reads its declaration, so each block is rejected at load regardless of the variant.
BOOST_AUTO_TEST_CASE(test_AnalysisLoad_RefusesMalformedAlignExports) {
    Unit host;
    const std::string languages = ALIGN_LANGUAGES;
    const std::pair<const char *, std::string> cases[] = {
        // A language is an ISO 639-3 code, not a two-letter code.
        {"two-letters",
         R"("languages": [{"language": "zh", "scheme": "pinyin", "lyrics": "scheme",)"
         R"( "phonemes": ["a"]}], "defaultLanguage": "zh")"                                                      },
        // A scheme is lowercase letters and digits in groups joined by single hyphens.
        {"scheme-grammar",
         R"("languages": [{"language": "cmn", "scheme": "Pin Yin", "lyrics": "scheme",)"
         R"( "phonemes": ["a"]}], "defaultLanguage": "cmn")"                                                     },
        {"scheme-hyphens",
         R"("languages": [{"language": "cmn", "scheme": "pin--yin", "lyrics": "scheme",)"
         R"( "phonemes": ["a"]}], "defaultLanguage": "cmn")"                                                     },
        // The lyrics are written either in the scheme or as text.
        {"lyrics-form",
         R"("languages": [{"language": "cmn", "scheme": "pinyin", "lyrics": "romanized",)"
         R"( "phonemes": ["a"]}], "defaultLanguage": "cmn")"                                                     },
        // A language and scheme pair appears once, and its phonemes do not repeat.
        {"pair-twice",
         R"("languages": [{"language": "cmn", "scheme": "pinyin", "lyrics": "scheme",)"
         R"( "phonemes": ["a"]}, {"language": "cmn", "scheme": "pinyin", "lyrics": "text",)"
         R"( "phonemes": ["b"]}], "defaultLanguage": "cmn")"                                                     },
        {"phonemes-repeat",
         R"("languages": [{"language": "cmn", "scheme": "pinyin", "lyrics": "scheme",)"
         R"( "phonemes": ["a", "a"]}], "defaultLanguage": "cmn")"                                                },
        {"entry-key",          R"("languages": [{"language": "cmn", "scheme": "pinyin", "lyrics": "scheme",)"
                      R"( "phonemes": ["a"], "script": "latin"}], "defaultLanguage": "cmn")"},
        // Listed languages need a default among them.
        {"no-default",
         R"("languages": [{"language": "cmn", "scheme": "pinyin", "lyrics": "scheme",)"
         R"( "phonemes": ["a"]}])"                                                                               },
        {"default-unlisted",   languages + R"(, "defaultLanguage": "eng")"                                       },
        // The default non-speech labels are among the declared ones.
        {"non-speech-default", languages + R"(, "defaultLanguage": "cmn", )"
                                           R"("nonSpeechPhonemes": ["AP"], )"
                                           R"("defaultNonSpeechPhonemes": ["EP"])" },
    };
    for (const auto &[name, exports] : cases) {
        BOOST_TEST_CONTEXT(name) {
            auto opened = host.unit.openPackage(writeAligner(name, exports), srt::SynthUnit::Load);
            BOOST_CHECK(!opened);
        }
    }

    // The well-formed declaration, from which every case differs, loads.
    auto opened = host.unit.openPackage(
        writeAligner("well-formed", languages + R"(, "defaultLanguage": "cmn")"),
        srt::SynthUnit::Load);
    BOOST_CHECK_MESSAGE(static_cast<bool>(opened), otter::test::why(opened));
}

BOOST_AUTO_TEST_SUITE_END()
