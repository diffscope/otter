// Creating and running an analyzer: the path from a loaded declaration to a result, the knobs
// that change one, and what happens when the caller gets the audio wrong or changes its mind.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include <synthrt/Core/ContribSpecExtension.h>
#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>

#include <otter/Analysis/AnalysisContrib.h>
#include <otter/Analysis/AnalysisExecutive.h>
#include <otter/Api/F0/1/F0ApiL1.h>
#include <otter/Api/Note/1/NoteApiL1.h>

#include "TestSupport.h"

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace {

    namespace fs = std::filesystem;
    namespace F0Api = otter::Api::F0::L1;
    namespace NoteApi = otter::Api::Note::L1;
    namespace CommonApi = otter::Api::Common::L1;

    constexpr int RATE = 16000;

    CommonApi::AudioSegment silence(double seconds, double startTime = 0, int rate = RATE,
                                    int channels = 1) {
        CommonApi::AudioSegment audio;
        audio.sampleRate = rate;
        audio.channelCount = channels;
        audio.samples.assign(static_cast<std::size_t>(seconds * rate * channels), 0.0f);
        audio.startTime = startTime;
        return audio;
    }

    /// A loaded package plus the unit behind it, torn down in the right order.
    struct Loaded {
        explicit Loaded(const char *name) {
            const fs::path paths[] = {fs::path(OTTER_TEST_PLUGIN_DIR)};
            unit.setPluginPaths(otter::ANALYSIS_CATEGORY, paths);
            auto opened =
                unit.openPackage(fs::path(OTTER_TEST_PACKAGE_DIR) / name, srt::SynthUnit::Load);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), otter::test::why(opened));
            package = opened.take();
        }

        ~Loaded() {
            package.reset();
        }

        template <class Executive>
        std::unique_ptr<Executive> create(const char *id) {
            auto *spec = package.contribution(otter::ANALYSIS_CATEGORY, id);
            BOOST_REQUIRE(spec != nullptr);
            auto *extension = srt::ContribSpecExtension::findFromSpec<Executive>(
                *spec->as<otter::AnalysisSpec>());
            BOOST_REQUIRE(extension != nullptr);
            auto options = RuntimeOptionsFor<Executive>();
            auto made = extension->template as<otter::AnalysisExtension>()->createAnalyzer(options);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
            auto executive = made.take();
            auto *typed = executive->template as<Executive>();
            BOOST_REQUIRE(typed != nullptr);
            executive.release();
            return std::unique_ptr<Executive>(typed);
        }

        srt::SynthUnit unit;
        srt::PackageHandle package;

    private:
        template <class Executive>
        static auto RuntimeOptionsFor() {
            if constexpr (std::is_same_v<Executive, F0Api::F0Executive>) {
                return F0Api::F0RuntimeOptions("stub");
            } else {
                return NoteApi::NoteRuntimeOptions("stub");
            }
        }
    };

}

BOOST_AUTO_TEST_SUITE(test_AnalysisRuntime)

BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_AnchorsTheCurveWhereTheHostSaidItWas) {
    Loaded loaded("stub-analyzers");
    auto analyzer = loaded.create<F0Api::F0Executive>("f0");

    F0Api::F0StartInput input;
    input.audio = silence(2.0, 30.5);
    std::vector<double> reported;
    input.progress = [&reported](double value) { reported.push_back(value); };

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    // Absolute seconds, not an offset into the span: calling once with the whole recording and
    // calling once per slice must produce the same numbers.
    BOOST_CHECK_CLOSE(result->startTime, 30.5, 1e-9);
    BOOST_CHECK_CLOSE(result->interval, 0.01, 1e-9);
    BOOST_CHECK_EQUAL(result->f0.size(), 200u);
    BOOST_CHECK_EQUAL(result->voiced.size(), result->f0.size());
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
    BOOST_REQUIRE_EQUAL(reported.size(), 2u);
    BOOST_CHECK_CLOSE(reported.front(), 0.0, 1e-9);
    BOOST_CHECK_CLOSE(reported.back(), 1.0, 1e-9);
}

BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_LeavingAKnobEmptyTakesTheModuleDefault) {
    Loaded loaded("stub-analyzers");
    auto analyzer = loaded.create<F0Api::F0Executive>("f0");

    F0Api::F0StartInput off;
    off.audio = silence(1.0);
    off.interpolateUnvoiced = false;
    auto withoutFill = analyzer->start(off);
    BOOST_REQUIRE(withoutFill);
    auto bare = withoutFill.take();

    F0Api::F0StartInput implied;
    implied.audio = silence(1.0);
    auto withDefault = analyzer->start(implied);
    BOOST_REQUIRE(withDefault);
    auto filled = withDefault.take();

    BOOST_REQUIRE_EQUAL(bare->f0.size(), filled->f0.size());
    bool sawUnvoiced = false;
    for (std::size_t i = 0; i < bare->f0.size(); ++i) {
        BOOST_REQUIRE_EQUAL(bare->voiced[i], filled->voiced[i]);
        if (!bare->voiced[i]) {
            sawUnvoiced = true;
            BOOST_CHECK_EQUAL(bare->f0[i], 0.0f);
            BOOST_CHECK_GT(filled->f0[i], 0.0f);
        }
    }
    BOOST_CHECK(sawUnvoiced);
}

BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_TranscribesAndHonorsTheCutoff) {
    Loaded loaded("stub-analyzers");
    auto analyzer = loaded.create<NoteApi::NoteExecutive>("note");

    NoteApi::NoteStartInput input;
    input.audio = silence(2.0, 10.0);
    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto all = produced.take();
    BOOST_REQUIRE(!all->notes.empty());
    // The declaration sets a quarter second beat, so eight notes fit in two seconds and the first
    // one sits where the host said the audio began.
    BOOST_CHECK_EQUAL(all->notes.size(), 8u);
    BOOST_CHECK_CLOSE(all->notes.front().start, 10.0, 1e-9);
    BOOST_CHECK_CLOSE(all->notes.back().start + all->notes.back().duration, 12.0, 1e-9);

    NoteApi::NoteStartInput picky;
    picky.audio = silence(2.0, 10.0);
    picky.notePresenceCutoff = 0.6;
    auto fewer = analyzer->start(picky);
    BOOST_REQUIRE(fewer);
    auto kept = fewer.take();
    BOOST_CHECK_LT(kept->notes.size(), all->notes.size());
    for (const auto &note : kept->notes) {
        BOOST_CHECK_GE(note.confidence, 0.6);
    }
}

BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_KeepsTheBoundariesTheCallerAlreadyKnows) {
    Loaded loaded("stub-analyzers");
    auto analyzer = loaded.create<NoteApi::NoteExecutive>("note");

    NoteApi::NoteStartInput input;
    input.audio = silence(2.0, 4.0);
    input.knownNotes = {{0.0, 0.3}, {0.5, 0.7}, {1.5, 0.25}};

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    BOOST_REQUIRE_EQUAL(result->notes.size(), input.knownNotes.size());
    for (std::size_t i = 0; i < result->notes.size(); ++i) {
        BOOST_CHECK_CLOSE(result->notes[i].start, 4.0 + input.knownNotes[i].start, 1e-9);
        BOOST_CHECK_CLOSE(result->notes[i].duration, input.knownNotes[i].duration, 1e-9);
    }
}

BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_RefusesAudioItWasNotPromised) {
    Loaded loaded("stub-analyzers");
    auto analyzer = loaded.create<F0Api::F0Executive>("f0");

    // Resampling is the host's job, and an analyzer that quietly did it would be changing the
    // result without saying so.
    F0Api::F0StartInput wrongRate;
    wrongRate.audio = silence(1.0, 0, 44100);
    BOOST_CHECK(!analyzer->start(wrongRate));

    F0Api::F0StartInput empty;
    empty.audio = silence(0.0);
    BOOST_CHECK(!analyzer->start(empty));

    // maxSegmentDuration is a hard constraint rather than advice: the host slices, and a span
    // beyond what the model can encode fails instead of degrading.
    F0Api::F0StartInput tooLong;
    tooLong.audio = silence(61.0);
    BOOST_CHECK(!analyzer->start(tooLong));

    NoteApi::NoteStartInput unknownLanguage;
    unknownLanguage.audio = silence(1.0);
    unknownLanguage.language = "qqq";
    auto note = loaded.create<NoteApi::NoteExecutive>("note");
    BOOST_CHECK(!note->start(unknownLanguage));
}

BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_RunsAsynchronouslyAndCanBeCancelled) {
    Loaded loaded("slow-analyzer");
    auto analyzer = loaded.create<F0Api::F0Executive>("f0");

    auto input = std::make_shared<F0Api::F0StartInput>();
    input->audio = silence(1.0);

    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    bool succeeded = true;

    auto started = analyzer->startAsync(input, [&](srt::Expected<std::unique_ptr<F0Api::F0Result>>
                                                      result) {
        std::lock_guard guard(mutex);
        succeeded = static_cast<bool>(result);
        finished = true;
        done.notify_all();
    });
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(started), otter::test::why(started));

    BOOST_CHECK(analyzer->stop());
    {
        std::unique_lock guard(mutex);
        BOOST_REQUIRE(done.wait_for(guard, std::chrono::seconds(10), [&] { return finished; }));
    }
    BOOST_CHECK(!succeeded);
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Canceled);
    BOOST_CHECK(analyzer->waitForFinished());

    // A stop belongs to the execution it was aimed at. Leaving it set would poison the next one,
    // which is a cancellation the caller never asked for.
    F0Api::F0StartInput again;
    again.audio = silence(1.0);
    auto after = analyzer->start(again);
    BOOST_CHECK_MESSAGE(static_cast<bool>(after), otter::test::why(after));
}

BOOST_AUTO_TEST_SUITE_END()
