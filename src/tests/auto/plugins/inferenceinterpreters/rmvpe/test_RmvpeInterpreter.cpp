// The rmvpe provider against a real ONNX graph.
//
// The graph is a fixture with the real signature and arithmetic in place of trained weights. The
// tests therefore verify the provider's side of the contract: the correct tensors reach the model,
// the outputs arrive in the correct data types, the knobs reach the model, the voicing flag is
// inverted in the result, and the curve is anchored at the start time that the host states for
// the audio. The accuracy of the values requires the trained model and is not tested here.

#include <memory>
#include <string>
#include <vector>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/SVS/InferenceContrib.h>
#include <synthrt/SVS/InferenceInterpreterPlugin.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Api/F0/1/F0ApiL1.h>

#include <onnxutil/OnnxSupport.h>
#include <rmvpe/RmvpeInterpreter.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

#include "TestSupport.h"

namespace F0Api = otter::Api::F0::L1;
namespace CommonApi = otter::Api::Common::L1;

namespace {

    // The plugin that main.cpp of rmvpe declares, around the interpreter that this test
    // links. The test registers it as a static plugin with the plugin.json of rmvpe, so that
    // discovery selects this class rather than the library in the plugin tree.
    class RmvpePlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(const srt::ContribContractTriplet &triplet) override {
            if (auto served = otter::onnx::checkServed(
                    triplet, srt::ContribContractTriplet(F0Api::Contract.interfaceId(),
                                                         F0Api::Contract.level(),
                                                         otter::rmvpe::Triplet.variant()));
                !served) {
                return served.takeError();
            }
            return std::unique_ptr<srt::ContribInterpreter>(new otter::rmvpe::RmvpeInterpreter());
        }
    };

}

STDC_EXPORT_STATIC_PLUGIN(RmvpePlugin, srt::InferenceInterpreterPlugin::IID,
                          otter::test::pluginManifest())

namespace {

    constexpr int RATE = 16000;

    using FixturesOrSkip = otter::test::FixturesOrSkip;

    struct Host : otter::test::ModelHost {
        std::unique_ptr<F0Api::Executive> open() {
            auto spec = load("fixture-rmvpe", "f0");
            auto made = F0Api::Executive::create(*spec->as<srt::InferenceSpec>());
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
            return made.take();
        }
    };

    CommonApi::AudioSegment tone(double seconds, double startTime = 0, int channels = 1) {
        CommonApi::AudioSegment audio;
        audio.sampleRate = RATE;
        audio.channelCount = channels;
        audio.samples.assign(static_cast<std::size_t>(seconds * RATE) * channels, 0.5f);
        audio.startTime = startTime;
        return audio;
    }

}

BOOST_TEST_GLOBAL_FIXTURE(FixturesOrSkip);

BOOST_AUTO_TEST_SUITE(test_RmvpeInterpreter)

BOOST_AUTO_TEST_CASE(test_RmvpeInterpreter_RunsTheModelAndAnchorsTheCurve) {
    Host host;
    auto analyzer = host.open();

    F0Api::StartInput input;
    input.audio = tone(1.0, 12.25);
    input.interpolateUnvoiced = false;

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    // One frame per 160 samples, which is the 10 ms the declaration reports.
    // The frame count follows the declaration's grid, which puts a frame every 10 ms. A span of
    // exactly one second lands on a frame boundary, and whether the last frame is emitted is the
    // model's own rounding.
    BOOST_CHECK_GE(result->f0.size(), 100u);
    BOOST_CHECK_LE(result->f0.size(), 101u);
    BOOST_CHECK_EQUAL(result->voiced.size(), result->f0.size());
    BOOST_CHECK_CLOSE(result->startTime, 12.25, 1e-9);
    BOOST_CHECK_CLOSE(result->interval, 0.01, 1e-9);

    // The fixture's curve is a ramp from 100 Hz, so frame order is visible in the values and a
    // provider that reversed or offset the buffer would show it here.
    BOOST_CHECK_EQUAL(result->f0.front(), 100.0f);

    // The model's flag marks unvoiced frames. The contract's marks voiced ones. With the
    // declaration's 0.03 threshold the fixture marks the first three frames of every hundred as
    // unvoiced, so the inversion is observed rather than assumed.
    BOOST_CHECK_EQUAL(static_cast<int>(result->voiced[0]), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(result->voiced[2]), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(result->voiced[3]), 0);
    BOOST_CHECK_EQUAL(result->f0[3], 0.0f);
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}

// The release package, when the host fetched one, runs the trained weights. The fixture case pins
// the arithmetic of the provider. This one proves that the weights load and that the curve keeps
// the frame grid the declaration promises.
BOOST_AUTO_TEST_CASE(test_RmvpeInterpreter_EstimatesWithTheTrainedModel) {
    Host host;
    auto spec = host.loadDirectory(otter::test::realModelsRoot() / "otter-rmvpe", "f0");
    auto made = F0Api::Executive::create(*spec->as<srt::InferenceSpec>());
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
    auto analyzer = made.take();

    F0Api::StartInput input;
    input.audio = tone(1.0, 2.0);
    input.interpolateUnvoiced = false;

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    // The frame count follows the declaration's grid, which puts a frame every 10 ms. A span of
    // exactly one second lands on a frame boundary, and whether the last frame is emitted is the
    // model's own rounding.
    BOOST_CHECK_GE(result->f0.size(), 100u);
    BOOST_CHECK_LE(result->f0.size(), 101u);
    BOOST_CHECK_EQUAL(result->voiced.size(), result->f0.size());
    BOOST_CHECK_CLOSE(result->startTime, 2.0, 1e-9);
    BOOST_CHECK_CLOSE(result->interval, 0.01, 1e-9);
    for (std::size_t i = 0; i < result->f0.size(); ++i) {
        // A frame the model did not voice carries no pitch, which is what the contract says an
        // unvoiced frame is.
        if (result->voiced[i]) {
            BOOST_CHECK_GT(result->f0[i], 0.0f);
        } else {
            BOOST_CHECK_EQUAL(result->f0[i], 0.0f);
        }
    }
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}
BOOST_AUTO_TEST_CASE(test_RmvpeInterpreter_SendsTheThresholdToTheModel) {
    Host host;
    auto analyzer = host.open();

    const auto voicedCount = [&analyzer](double threshold) {
        F0Api::StartInput input;
        input.audio = tone(1.0);
        input.voicingThreshold = threshold;
        auto produced = analyzer->start(input);
        BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
        auto result = produced.take();
        std::size_t count = 0;
        for (auto flag : result->voiced) {
            count += flag ? 1 : 0;
        }
        return count;
    };

    // If the knob did not reach the model, the two counts would be equal. A check of the
    // declaration alone cannot detect this failure.
    BOOST_CHECK_LT(voicedCount(0.1), voicedCount(0.5));
}

BOOST_AUTO_TEST_CASE(test_RmvpeInterpreter_InterpolationFillsWhatTheFlagLeavesOut) {
    Host host;
    auto analyzer = host.open();

    F0Api::StartInput raw;
    raw.audio = tone(1.0);
    raw.voicingThreshold = 0.5;
    raw.interpolateUnvoiced = false;
    auto without = analyzer->start(raw);
    BOOST_REQUIRE(without);
    auto bare = without.take();

    F0Api::StartInput filled;
    filled.audio = tone(1.0);
    filled.voicingThreshold = 0.5;
    filled.interpolateUnvoiced = true;
    auto with = analyzer->start(filled);
    BOOST_REQUIRE(with);
    auto interpolated = with.take();

    BOOST_REQUIRE_EQUAL(bare->voiced.size(), interpolated->voiced.size());
    bool sawFilled = false;
    for (std::size_t i = 0; i < bare->voiced.size(); ++i) {
        // Interpolation changes the curve but never the flag, so a host can still identify the
        // frames that carried a measurement.
        BOOST_REQUIRE_EQUAL(bare->voiced[i], interpolated->voiced[i]);
        if (!bare->voiced[i]) {
            BOOST_CHECK_EQUAL(bare->f0[i], 0.0f);
            BOOST_CHECK_GT(interpolated->f0[i], 0.0f);
            sawFilled = true;
        }
    }
    BOOST_CHECK(sawFilled);
}

BOOST_AUTO_TEST_CASE(test_RmvpeInterpreter_DownmixesButRefusesToResample) {
    Host host;
    auto analyzer = host.open();

    // Averaging channels is plain arithmetic, so the provider performs it instead of requiring
    // the caller to do so.
    F0Api::StartInput stereo;
    stereo.audio = tone(1.0, 0, 2);
    auto mixed = analyzer->start(stereo);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(mixed), otter::test::why(mixed));
    BOOST_CHECK_EQUAL(mixed.take()->f0.size(), 100u);

    // Resampling is not performed, because it changes the result, and silent resampling would hide
    // that the host prepared audio at the wrong rate.
    F0Api::StartInput wrongRate;
    wrongRate.audio = tone(1.0);
    wrongRate.audio.sampleRate = 44100;
    auto refused = analyzer->start(wrongRate);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().message().find("16000") != std::string::npos);
}

// The graph is built for one rate and one hop. A declaration that states other values would make
// the host prepare audio that the model then misplaces, so the declaration is rejected at load
// time.
BOOST_AUTO_TEST_CASE(test_RmvpeInterpreter_RefusesADeclarationTheModelCannotHonor) {
    Host host;
    BOOST_CHECK(host.refusal("fixture-rmvpe-wrong-rate").find("16000") != std::string::npos);
}

// The graph takes one channel, and the variant mixes nothing down: a declaration of two describes
// audio that a host prepares and the provider would silently reduce, so the count is checked where
// the declaration is read. No value of the graph itself could contradict the declaration here,
// which is why this package writes the same graph as the good fixture.
BOOST_AUTO_TEST_CASE(test_RmvpeInterpreter_RefusesADeclarationOfTwoChannels) {
    Host host;
    const auto refusal = host.refusalError("fixture-rmvpe-two-channels");
    BOOST_CHECK(refusal.code() == srt::Error::InvalidFormat);
    BOOST_CHECK(refusal.message().find("the rmvpe variant feeds its model one channel") !=
                std::string::npos);
    BOOST_CHECK(refusal.message().find("declare 2") != std::string::npos);
}

// A stop requested during an execution makes it report Cancelled and no result, and it does not
// carry over to the next execution. The stop is requested from the progress callback, which the
// provider calls on the executing thread before the model runs, so the timing is not left to a
// race with a model that takes milliseconds.
BOOST_AUTO_TEST_CASE(test_RmvpeInterpreter_ReportsACancellation) {
    Host host;
    auto analyzer = host.open();

    F0Api::StartInput input;
    input.audio = tone(1.0);
    input.progress = [&analyzer](double value) {
        // Zero is the report that the provider makes before the model runs (the executive in
        // rmvpe/RmvpeInterpreter.cpp).
        if (value == 0) {
            (void) analyzer->stop();
        }
        return true;
    };
    auto stopped = analyzer->start(input);
    BOOST_REQUIRE(!stopped);
    BOOST_CHECK(stopped.error().code() == otter::AnalysisError::Cancelled);
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Canceled);

    F0Api::StartInput again;
    again.audio = tone(1.0);
    auto produced = analyzer->start(again);
    BOOST_CHECK_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}

BOOST_AUTO_TEST_SUITE_END()
