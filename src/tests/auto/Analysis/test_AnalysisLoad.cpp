// Loading an analysis package: what the category accepts, what the provider reads out of a
// declaration, and what a host can see afterwards without having created anything.

#include <filesystem>
#include <string>
#include <vector>

#include <synthrt/Core/ContribSpecExtension.h>
#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>

#include <otter/Analysis/AnalysisContrib.h>
#include <otter/Analysis/AnalysisExecutive.h>
#include <otter/Analysis/AnalysisProviderPlugin.h>
#include <otter/Api/F0/1/F0ApiL1.h>
#include <otter/Api/Note/1/NoteApiL1.h>

#include "TestSupport.h"

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace {

    namespace fs = std::filesystem;
    namespace F0Api = otter::Api::F0::L1;
    namespace NoteApi = otter::Api::Note::L1;

    fs::path packages() {
        return fs::path(OTTER_TEST_PACKAGE_DIR);
    }

    /// A unit wired the way a host wires one: the stub provider on the analysis plugin path.
    struct Unit {
        Unit() {
            // What every host does once: name the library, so a linker that drops unreferenced
            // libraries keeps the one whose static initializer registers the category.
            otter::linkAnalysisCategory();
            const fs::path paths[] = {fs::path(OTTER_TEST_PLUGIN_DIR)};
            unit.setPluginPaths(otter::ANALYSIS_CATEGORY, paths);
        }

        srt::SynthUnit unit;
    };

}

BOOST_AUTO_TEST_SUITE(test_AnalysisLoad)

BOOST_AUTO_TEST_CASE(test_AnalysisLoad_ReadsBothContracts) {
    Unit host;
    auto opened = host.unit.openPackage(packages() / "stub-analyzers", srt::SynthUnit::Load);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), otter::test::why(opened));
    auto package = opened.take();

    auto *category = host.unit.category(otter::ANALYSIS_CATEGORY)->as<otter::AnalysisCategory>();
    BOOST_REQUIRE_EQUAL(category->analyzers().size(), 2u);

    auto *f0 = package.contribution(otter::ANALYSIS_CATEGORY, "f0");
    BOOST_REQUIRE(f0 != nullptr);
    BOOST_CHECK_EQUAL(f0->interface(), F0Api::API_INTERFACE);
    BOOST_CHECK_EQUAL(f0->level(), F0Api::API_LEVEL);
    BOOST_CHECK_EQUAL(f0->variant(), "stub");
    BOOST_CHECK_EQUAL(f0->name().text(), "Stub F0");

    // What the host reads before it commits to anything: the audio format it must produce, and
    // which knobs this module answers to.
    const auto *schema = f0->exports()->as<F0Api::F0Schema>();
    BOOST_REQUIRE(schema != nullptr);
    BOOST_CHECK_EQUAL(schema->sampleRate, 16000);
    BOOST_CHECK_EQUAL(schema->channelCount, 1);
    BOOST_CHECK_CLOSE(schema->interval, 0.01, 1e-9);
    BOOST_CHECK_CLOSE(schema->maxSegmentDuration, 60.0, 1e-9);
    BOOST_CHECK(schema->voicingThreshold.honored);
    BOOST_CHECK_CLOSE(schema->voicingThreshold.defaultValue, 0.03, 1e-9);
    BOOST_CHECK(schema->interpolateUnvoiced.honored);

    auto *note = package.contribution(otter::ANALYSIS_CATEGORY, "note");
    BOOST_REQUIRE(note != nullptr);
    BOOST_CHECK_EQUAL(note->interface(), NoteApi::API_INTERFACE);
    const auto *noteSchema = note->exports()->as<NoteApi::NoteSchema>();
    BOOST_REQUIRE(noteSchema != nullptr);
    BOOST_CHECK(noteSchema->supportsKnownNotes);
    BOOST_REQUIRE_EQUAL(noteSchema->languages.size(), 1u);
    BOOST_CHECK_EQUAL(noteSchema->languages.front(), "zxx");
    BOOST_CHECK(noteSchema->steps.honored);
    BOOST_CHECK_EQUAL(noteSchema->steps.defaultValue, 8);

    // One extension per contract, and each is keyed on its own contract rather than on the
    // category: this is what lets Align and Transcribe arrive later without disturbing these two.
    BOOST_CHECK(srt::ContribSpecExtension::findFromSpec<F0Api::F0Executive>(
                    *f0->as<otter::AnalysisSpec>()) != nullptr);
    BOOST_CHECK(srt::ContribSpecExtension::findFromSpec<NoteApi::NoteExecutive>(
                    *f0->as<otter::AnalysisSpec>()) == nullptr);
    BOOST_CHECK(srt::ContribSpecExtension::findFromSpec<NoteApi::NoteExecutive>(
                    *note->as<otter::AnalysisSpec>()) != nullptr);
}

BOOST_AUTO_TEST_CASE(test_AnalysisLoad_DataOnlyStopsBeforeTheProvider) {
    // A host listing installed analyzers in a settings page needs the name, the contract and the
    // variant, and none of those needs an interpreter. Reading the format the model wants does.
    Unit host;
    auto opened = host.unit.openPackage(packages() / "stub-analyzers", srt::SynthUnit::DataOnly);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), otter::test::why(opened));
    auto package = opened.take();

    auto *f0 = package.contribution(otter::ANALYSIS_CATEGORY, "f0");
    BOOST_REQUIRE(f0 != nullptr);
    BOOST_CHECK_EQUAL(f0->interface(), F0Api::API_INTERFACE);
    BOOST_CHECK_EQUAL(f0->variant(), "stub");
    BOOST_CHECK(f0->exports() == nullptr);
    // The declaration's own words are still there for a host that wants to read them without a
    // provider, which is what putting the audio format in exports rather than configuration buys.
    const auto &declared = f0->manifestExports();
    BOOST_REQUIRE(declared.isObject());
    BOOST_CHECK_EQUAL(declared.toObject().at("sampleRate").toInt(), 16000);
}

/// The exports block is read by one reader for every variant, so what it refuses is refused the
/// same way everywhere: a missing audio format, a key the contract does not define, and a knob
/// whose default lies outside its own range.
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
    // A misspelt key means the value the author meant to set is silently absent, and for a
    // threshold that reads as a working model producing the wrong answer.
    Unit host;
    auto opened = host.unit.openPackage(packages() / "bad-configuration", srt::SynthUnit::Load);
    BOOST_REQUIRE(!opened);
    BOOST_CHECK(opened.error().rootCause().message().find("frequencey") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_AnalysisLoad_LetsAnotherModuleReferenceAnAnalyzer) {
    // The upper specification lets a category supply no Executive Factory, and the Loader accepts
    // a null one, so a package that references an analyzer is legal and must load. Refusing the
    // import outright — which is what this category did first — turns someone else's valid
    // package into a load failure.
    Unit host;
    auto opened = host.unit.openPackage(packages() / "importing", srt::SynthUnit::Load);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), otter::test::why(opened));
    auto package = opened.take();
    auto *note = package.contribution(otter::ANALYSIS_CATEGORY, "note");
    BOOST_REQUIRE(note != nullptr);
    BOOST_REQUIRE_EQUAL(note->imports().size(), 1u);
    BOOST_CHECK(note->findImport("analysis/reference").has_value());
    package.reset();
}

BOOST_AUTO_TEST_CASE(test_AnalysisLoad_RejectsImportOptionsNothingWillRead) {
    // Absent options are one thing; written ones are a statement the author expects to take
    // effect, and no contract in this category reads any.
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

BOOST_AUTO_TEST_SUITE_END()
