// The game provider against real ONNX graphs.
//
// Five models with fixture weights, wired to arithmetic a test can predict. What this proves is
// the provider's half: every declared input reaches the model it belongs to, the four stages are
// chained in the right order, the knobs get through, the notes come out on the host's timeline in
// seconds, and the alignment path actually runs the fifth model rather than passing zeros.

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <dsinfer/Api/Drivers/Onnx/OnnxDriverApi.h>
#include <dsinfer/Inference/InferenceDriverFactory.h>

#include <synthrt/Core/ContribSpecExtension.h>
#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>

#include <otter/Analysis/AnalysisContrib.h>
#include <otter/Analysis/AnalysisExecutive.h>
#include <otter/Api/Note/1/NoteApiL1.h>

#include "TestSupport.h"

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace fs = std::filesystem;
namespace NoteApi = otter::Api::Note::L1;
namespace CommonApi = otter::Api::Common::L1;
namespace OnnxApi = ds::Api::Onnx;

namespace {

    constexpr int RATE = 44100;

    /// The models answer in float32, so a duration of a fifth of a second comes back as
    /// 0.19999998. BOOST_CHECK_CLOSE takes a percentage, and this one is a float's worth of it.
    constexpr double TOLERANCE = 1e-4;

    bool fixturePresent() {
        return fs::is_directory(fs::path(OTTER_TEST_FIXTURE_DIR) / "fixture-note");
    }

    struct Host {
        Host() {
            std::vector<fs::path> driverPaths = {fs::path(OTTER_TEST_DRIVER_PLUGIN_DIR)};
            factory.setPluginPaths(driverPaths);
            auto *loader = factory.find(OnnxApi::API_NAME);
            if (loader == nullptr) {
                BOOST_TEST_MESSAGE("no ONNX driver plugin present");
                return;
            }
            auto created = factory.create(loader);
            if (!created) {
                BOOST_TEST_MESSAGE("the driver could not be created");
                return;
            }
            auto driver = created.take();
            OnnxApi::DriverInitArgs args;
            args.ep = OnnxApi::ExecutionProvider::CPU;
            args.runtimePath = fs::path(OTTER_TEST_ONNXRUNTIME_DIR);
            if (auto initialized = driver->initialize(args); !initialized) {
                BOOST_TEST_MESSAGE("the driver could not be initialized");
                return;
            }
            auto added = unit.addRuntimeService(std::move(driver));
            BOOST_REQUIRE_MESSAGE(added, "the driver should have been registered");

            std::vector<fs::path> pluginPaths = {fs::path(OTTER_TEST_PLUGIN_DIR)};
            unit.setPluginPaths(otter::ANALYSIS_CATEGORY, pluginPaths);
            ready = true;
        }

        ~Host() {
            package.reset();
        }

        srt::ContribSpec *load() {
            auto opened = unit.openPackage(fs::path(OTTER_TEST_FIXTURE_DIR) / "fixture-note",
                                           srt::SynthUnit::Load);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), otter::test::why(opened));
            package = opened.take();
            auto *spec = package.contribution(otter::ANALYSIS_CATEGORY, "note");
            BOOST_REQUIRE(spec != nullptr);
            return spec;
        }

        std::unique_ptr<NoteApi::NoteExecutive> open() {
            auto *spec = load();
            auto *extension = srt::ContribSpecExtension::findFromSpec<NoteApi::NoteExecutive>(
                *spec->as<otter::AnalysisSpec>());
            BOOST_REQUIRE(extension != nullptr);
            NoteApi::NoteRuntimeOptions options("game");
            auto made = extension->as<otter::AnalysisExtension>()->createAnalyzer(options);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
            auto executive = made.take();
            auto *typed = executive->as<NoteApi::NoteExecutive>();
            BOOST_REQUIRE(typed != nullptr);
            executive.release();
            return std::unique_ptr<NoteApi::NoteExecutive>(typed);
        }

        ds::InferenceDriverFactory factory;
        srt::SynthUnit unit;
        srt::PackageHandle package;
        bool ready = false;
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

BOOST_AUTO_TEST_SUITE(test_Game)

BOOST_AUTO_TEST_CASE(test_Game_ReportsWhatTheConfigurationSays) {
    if (!fixturePresent()) {
        BOOST_TEST_MESSAGE("no model fixture; run scripts/make-model-fixtures.py");
        return;
    }
    Host host;
    if (!host.ready) {
        return;
    }
    auto *spec = host.load();
    const auto *schema = spec->exports()->as<NoteApi::NoteSchema>();
    BOOST_REQUIRE(schema != nullptr);

    // The exports are derived from the configuration rather than declared beside it, so what the
    // host prepares and what the model opens cannot drift apart.
    BOOST_CHECK_EQUAL(schema->sampleRate, RATE);
    BOOST_CHECK_CLOSE(schema->maxSegmentDuration, 60.0, 1e-9);
    BOOST_CHECK(schema->supportsKnownNotes);
    BOOST_REQUIRE_EQUAL(schema->languages.size(), 2u);
    BOOST_CHECK(schema->steps.honored);
    BOOST_CHECK_EQUAL(schema->steps.defaultValue, 8);
    BOOST_CHECK_CLOSE(schema->boundaryRadius.defaultValue, 0.2, 1e-9);
}

BOOST_AUTO_TEST_CASE(test_Game_ChainsTheStagesAndPlacesNotesInSeconds) {
    if (!fixturePresent()) {
        return;
    }
    Host host;
    if (!host.ready) {
        return;
    }
    auto analyzer = host.open();

    NoteApi::NoteStartInput input;
    input.audio = tone(2.0, 7.5);
    input.notePresenceCutoff = 0.0;
    std::vector<double> reported;
    input.progress = [&reported](double value) { reported.push_back(value); };

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    // Two seconds at a 10 ms frame is 200 frames; the declaration's 0.2 s radius makes a boundary
    // every 20 frames, so ten notes of 0.2 s each. Reaching that number at all means all four
    // stages ran and were chained in the right order.
    BOOST_REQUIRE_EQUAL(result->notes.size(), 10u);
    BOOST_CHECK_CLOSE(result->notes.front().start, 7.5, TOLERANCE);
    BOOST_CHECK_CLOSE(result->notes.front().duration, 0.2, TOLERANCE);

    // Contiguous and in order, in absolute seconds on the host's timeline.
    for (std::size_t i = 1; i < result->notes.size(); ++i) {
        BOOST_CHECK_CLOSE(result->notes[i].start,
                          result->notes[i - 1].start + result->notes[i - 1].duration, TOLERANCE);
    }
    BOOST_CHECK_CLOSE(result->notes.back().start + result->notes.back().duration, 9.5, TOLERANCE);

    // The fixture estimator answers with a chromatic run from middle C, so note order is visible.
    BOOST_CHECK_EQUAL(result->notes.front().key, 60);
    BOOST_CHECK_EQUAL(result->notes[1].key, 61);

    BOOST_REQUIRE(!reported.empty());
    BOOST_CHECK_CLOSE(reported.back(), 1.0, 1e-9);
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}

BOOST_AUTO_TEST_CASE(test_Game_SendsTheKnobsToTheModels) {
    if (!fixturePresent()) {
        return;
    }
    Host host;
    if (!host.ready) {
        return;
    }
    auto analyzer = host.open();

    const auto count = [&analyzer](double radius, double cutoff) {
        NoteApi::NoteStartInput input;
        input.audio = tone(2.0);
        input.boundaryRadius = radius;
        input.notePresenceCutoff = cutoff;
        auto produced = analyzer->start(input);
        BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
        return produced.take()->notes.size();
    };

    // A narrower radius means more boundaries, so more notes: the knob reached the segmenter.
    BOOST_CHECK_GT(count(0.1, 0.0), count(0.4, 0.0));
    // The cutoff filters the estimator's output, and the fixture alternates confidences, so
    // raising it past the low one halves the count.
    BOOST_CHECK_LT(count(0.2, 0.6), count(0.2, 0.0));
}

BOOST_AUTO_TEST_CASE(test_Game_AlignsAgainstTheNotesTheCallerKnows) {
    if (!fixturePresent()) {
        return;
    }
    Host host;
    if (!host.ready) {
        return;
    }
    auto analyzer = host.open();

    NoteApi::NoteStartInput free;
    free.audio = tone(2.0);
    free.boundaryRadius = 0.4;
    free.notePresenceCutoff = 0.0;
    auto without = analyzer->start(free);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(without), otter::test::why(without));
    const auto unconditioned = without.take()->notes.size();

    // The same audio, but told where the notes are. The fifth model turns those durations into
    // boundaries the segmenter keeps, so the answer must change — which it could not do while the
    // segmenter was being handed zeros, as it was in the implementation this came from.
    NoteApi::NoteStartInput aligned;
    aligned.audio = tone(2.0);
    aligned.boundaryRadius = 0.4;
    aligned.notePresenceCutoff = 0.0;
    aligned.knownNotes = {{0.0, 0.1}, {0.1, 0.1}, {0.2, 0.1}, {0.3, 0.1}, {0.4, 0.1}};
    auto with = analyzer->start(aligned);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(with), otter::test::why(with));
    const auto conditioned = with.take()->notes.size();

    BOOST_CHECK_GT(conditioned, unconditioned);
}

BOOST_AUTO_TEST_CASE(test_Game_RefusesWhatItCannotHonor) {
    if (!fixturePresent()) {
        return;
    }
    Host host;
    if (!host.ready) {
        return;
    }
    auto analyzer = host.open();

    NoteApi::NoteStartInput wrongRate;
    wrongRate.audio = tone(1.0);
    wrongRate.audio.sampleRate = 16000;
    auto refusedRate = analyzer->start(wrongRate);
    BOOST_REQUIRE(!refusedRate);
    BOOST_CHECK(refusedRate.error().message().find("44100") != std::string::npos);

    NoteApi::NoteStartInput unknownLanguage;
    unknownLanguage.audio = tone(1.0);
    unknownLanguage.language = "qqq";
    BOOST_CHECK(!analyzer->start(unknownLanguage));

    // Out of order known notes would place every later note early rather than fail, so they are
    // refused instead.
    NoteApi::NoteStartInput crossed;
    crossed.audio = tone(1.0);
    crossed.knownNotes = {{0.5, 0.2}, {0.1, 0.2}};
    BOOST_CHECK(!analyzer->start(crossed));
}

BOOST_AUTO_TEST_SUITE_END()
