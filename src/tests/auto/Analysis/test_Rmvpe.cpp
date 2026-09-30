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

#include <synthrt/SVS/InferenceContrib.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Api/F0/1/F0ApiL1.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

#include "TestSupport.h"

namespace F0Api = otter::Api::F0::L1;
namespace CommonApi = otter::Api::Common::L1;

namespace {

    constexpr int RATE = 16000;

    using FixturesOrSkip = otter::test::FixturesOrSkip;

    struct Host : otter::test::ModelHost {
        std::unique_ptr<F0Api::F0Executive> open() {
            auto spec = load("fixture-rmvpe", "f0");
            auto made = F0Api::createAnalyzer(*spec->as<srt::InferenceSpec>());
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

BOOST_AUTO_TEST_SUITE(test_Rmvpe)

BOOST_AUTO_TEST_CASE(test_Rmvpe_RunsTheModelAndAnchorsTheCurve) {
    Host host;
    auto analyzer = host.open();

    F0Api::F0StartInput input;
    input.audio = tone(1.0, 12.25);
    input.interpolateUnvoiced = false;

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    // One frame per 160 samples, which is the 10 ms the declaration reports.
    BOOST_CHECK_EQUAL(result->f0.size(), 100u);
    BOOST_CHECK_EQUAL(result->voiced.size(), result->f0.size());
    BOOST_CHECK_CLOSE(result->startTime, 12.25, 1e-9);
    BOOST_CHECK_CLOSE(result->interval, 0.01, 1e-9);

    // The fixture's curve is a ramp from 100 Hz, so frame order is visible in the values and a
    // provider that reversed or offset the buffer would show it here.
    BOOST_CHECK_EQUAL(result->f0.front(), 100.0f);

    // The model's flag marks unvoiced frames; the contract's marks voiced ones. With the
    // declaration's 0.03 threshold the fixture marks the first three frames of every hundred as
    // unvoiced, so the inversion is observed rather than assumed.
    BOOST_CHECK_EQUAL(static_cast<int>(result->voiced[0]), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(result->voiced[2]), 1);
    BOOST_CHECK_EQUAL(static_cast<int>(result->voiced[3]), 0);
    BOOST_CHECK_EQUAL(result->f0[3], 0.0f);
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}

BOOST_AUTO_TEST_CASE(test_Rmvpe_SendsTheThresholdToTheModel) {
    Host host;
    auto analyzer = host.open();

    const auto voicedCount = [&analyzer](double threshold) {
        F0Api::F0StartInput input;
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

    // If the knob did not reach the model, the two counts would be equal; a check of the
    // declaration alone cannot detect this failure.
    BOOST_CHECK_LT(voicedCount(0.1), voicedCount(0.5));
}

BOOST_AUTO_TEST_CASE(test_Rmvpe_InterpolationFillsWhatTheFlagLeavesOut) {
    Host host;
    auto analyzer = host.open();

    F0Api::F0StartInput raw;
    raw.audio = tone(1.0);
    raw.voicingThreshold = 0.5;
    raw.interpolateUnvoiced = false;
    auto without = analyzer->start(raw);
    BOOST_REQUIRE(without);
    auto bare = without.take();

    F0Api::F0StartInput filled;
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

BOOST_AUTO_TEST_CASE(test_Rmvpe_DownmixesButRefusesToResample) {
    Host host;
    auto analyzer = host.open();

    // Averaging channels is plain arithmetic, so the provider performs it instead of requiring
    // the caller to do so.
    F0Api::F0StartInput stereo;
    stereo.audio = tone(1.0, 0, 2);
    auto mixed = analyzer->start(stereo);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(mixed), otter::test::why(mixed));
    BOOST_CHECK_EQUAL(mixed.take()->f0.size(), 100u);

    // Resampling is not performed, because it changes the result, and silent resampling would hide
    // that the host prepared audio at the wrong rate.
    F0Api::F0StartInput wrongRate;
    wrongRate.audio = tone(1.0);
    wrongRate.audio.sampleRate = 44100;
    auto refused = analyzer->start(wrongRate);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().message().find("16000") != std::string::npos);
}

/// The graph is built for one rate and one hop. A declaration that states other values would make
/// the host prepare audio that the model then misplaces, so the declaration is rejected at load
/// time.
BOOST_AUTO_TEST_CASE(test_Rmvpe_RefusesADeclarationTheModelCannotHonor) {
    Host host;
    BOOST_CHECK(host.refusal("fixture-rmvpe-wrong-rate").find("16000") != std::string::npos);
}

/// A stop requested during an execution makes it report Cancelled and no result, and it does not
/// carry over to the next execution. The stop is requested from the progress callback, which the
/// provider calls on the executing thread before the model runs, so the timing is not left to a
/// race with a model that takes milliseconds.
BOOST_AUTO_TEST_CASE(test_Rmvpe_ReportsACancellation) {
    Host host;
    auto analyzer = host.open();

    F0Api::F0StartInput input;
    input.audio = tone(1.0);
    input.progress = [&analyzer](double value) {
        if (value == 0) {
            (void) analyzer->stop();
        }
    };
    auto stopped = analyzer->start(input);
    BOOST_REQUIRE(!stopped);
    BOOST_CHECK(stopped.error().code() == otter::AnalysisError::Cancelled);
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Canceled);

    F0Api::F0StartInput again;
    again.audio = tone(1.0);
    auto produced = analyzer->start(again);
    BOOST_CHECK_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}

BOOST_AUTO_TEST_SUITE_END()
