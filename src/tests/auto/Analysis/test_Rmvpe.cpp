// The rmvpe provider against a real ONNX graph.
//
// The graph is a fixture with the real signature and arithmetic for weights, so what this proves
// is the provider's half of the contract: the right tensors reach the model, the outputs come back
// in the right dtypes, the knobs get through, the voicing flag is inverted on the way out, and the
// curve is anchored where the host said the audio was. Whether the numbers mean anything needs the
// trained model and is not asked here.

#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <iostream>
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
#include <otter/Api/F0/1/F0ApiL1.h>

#include "TestSupport.h"

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace fs = std::filesystem;
namespace F0Api = otter::Api::F0::L1;
namespace CommonApi = otter::Api::Common::L1;
namespace OnnxApi = ds::Api::Onnx;

namespace {

    constexpr int RATE = 16000;

    /// Exit status ctest reads as a skip, so a run without the generated graphs, the driver or
    /// the runtime is reported as not run rather than passed off as success.
    constexpr int SKIP_EXIT_CODE = 77;

    struct DataOrSkip {
        DataOrSkip() {
            if (!fs::is_directory(fs::path(OTTER_TEST_FIXTURE_DIR) / "fixture-rmvpe")) {
                std::cerr << "SKIP: no model fixture under " OTTER_TEST_FIXTURE_DIR "\n";
                std::exit(SKIP_EXIT_CODE);
            }
            if (!fs::is_directory(fs::path(OTTER_TEST_DRIVER_PLUGIN_DIR)) ||
                std::string_view(OTTER_TEST_ONNXRUNTIME_DIR).empty()) {
                std::cerr << "SKIP: no ONNX driver plugin or ONNX Runtime in this tree\n";
                std::exit(SKIP_EXIT_CODE);
            }
        }
    };

    /// Does what a host does: finds the ONNX driver, initializes it against a runtime directory
    /// the host names, and registers it as the backend the whole unit shares.
    struct Host {
        Host() {
            // What every host does once: name the library, so a linker that drops unreferenced
            // libraries keeps the one whose static initializer registers the category.
            otter::linkAnalysisCategory();
            std::vector<fs::path> driverPaths = {fs::path(OTTER_TEST_DRIVER_PLUGIN_DIR)};
            factory.setPluginPaths(driverPaths);
            auto *loader = factory.find(OnnxApi::API_NAME);
            BOOST_REQUIRE_MESSAGE(loader != nullptr, "the ONNX driver plugin should be present");
            auto created = factory.create(loader);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(created), otter::test::why(created));
            auto driver = created.take();
            OnnxApi::DriverInitArgs args;
            args.ep = OnnxApi::ExecutionProvider::CPU;
            args.runtimePath = fs::path(OTTER_TEST_ONNXRUNTIME_DIR);
            auto initialized = driver->initialize(args);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(initialized), otter::test::why(initialized));
            auto added = unit.addRuntimeService(std::move(driver));
            BOOST_REQUIRE_MESSAGE(added, "the driver should have been registered");

            std::vector<fs::path> pluginPaths = {fs::path(OTTER_TEST_PLUGIN_DIR)};
            unit.setPluginPaths(otter::ANALYSIS_CATEGORY, pluginPaths);
            ready = true;
        }

        ~Host() {
            package.reset();
        }

        std::unique_ptr<F0Api::F0Executive> open() {
            auto opened = unit.openPackage(fs::path(OTTER_TEST_FIXTURE_DIR) / "fixture-rmvpe",
                                           srt::SynthUnit::Load);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), otter::test::why(opened));
            package = opened.take();

            auto *spec = package.contribution(otter::ANALYSIS_CATEGORY, "f0");
            BOOST_REQUIRE(spec != nullptr);
            auto *extension = srt::ContribSpecExtension::findFromSpec<F0Api::F0Executive>(
                *spec->as<otter::AnalysisSpec>());
            BOOST_REQUIRE(extension != nullptr);

            F0Api::F0RuntimeOptions options("rmvpe");
            auto made = extension->as<otter::AnalysisExtension>()->createAnalyzer(options);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
            auto executive = made.take();
            auto *typed = executive->as<F0Api::F0Executive>();
            BOOST_REQUIRE(typed != nullptr);
            executive.release();
            return std::unique_ptr<F0Api::F0Executive>(typed);
        }

        ds::InferenceDriverFactory factory;
        srt::SynthUnit unit;
        srt::PackageHandle package;
        bool ready = false;
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

BOOST_TEST_GLOBAL_FIXTURE(DataOrSkip);

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
    // declaration's 0.03 threshold the fixture calls the first three frames of every hundred
    // unvoiced, so the inversion is visible rather than assumed.
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

    // A knob that never reached the model would make these equal, which is exactly the failure a
    // declaration-only check could not see.
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
        // Interpolation changes the curve, never the flag: a host is still told which frames
        // carried a measurement.
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

    // Averaging channels is arithmetic the caller would otherwise be asked to do for no reason.
    F0Api::F0StartInput stereo;
    stereo.audio = tone(1.0, 0, 2);
    auto mixed = analyzer->start(stereo);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(mixed), otter::test::why(mixed));
    BOOST_CHECK_EQUAL(mixed.take()->f0.size(), 100u);

    // Resampling is not: it changes the result, and doing it silently would hide that the host
    // prepared the wrong audio.
    F0Api::F0StartInput wrongRate;
    wrongRate.audio = tone(1.0);
    wrongRate.audio.sampleRate = 44100;
    auto refused = analyzer->start(wrongRate);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().message().find("16000") != std::string::npos);
}

/// The graph is built for one rate and one hop. A declaration that promises another would make the
/// host prepare audio the model then misplaces, so it is refused at load rather than trusted.
BOOST_AUTO_TEST_CASE(test_Rmvpe_RefusesADeclarationTheModelCannotHonor) {
    Host host;
    auto opened = host.unit.openPackage(
        fs::path(OTTER_TEST_FIXTURE_DIR) / "fixture-rmvpe-wrong-rate", srt::SynthUnit::Load);
    BOOST_REQUIRE(!opened);
    BOOST_CHECK(opened.error().rootCause().message().find("16000") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
