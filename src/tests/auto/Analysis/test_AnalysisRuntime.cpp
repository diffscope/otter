// Creating and running an analyzer: the path from a loaded declaration to a result, the knobs that
// change a result, and the behavior for invalid audio and for cancellation.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/SVS/InferenceContrib.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/AnalysisExecutive.h>
#include <otter/Analysis/AnalysisInput.h>
#include <otter/Analysis/AnalysisTask.h>
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
            unit.setPluginPaths(srt::InferenceCategory::NAME, paths);
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
            auto spec = package.contribution(srt::InferenceCategory::NAME, id);
            BOOST_REQUIRE(spec != nullptr);
            auto &inference = *spec->as<srt::InferenceSpec>();
            auto made = [&] {
                if constexpr (std::is_same_v<Executive, F0Api::F0Executive>) {
                    return F0Api::createAnalyzer(inference);
                } else if constexpr (std::is_same_v<Executive, AlignApi::AlignExecutive>) {
                    return AlignApi::createAnalyzer(inference);
                } else {
                    return NoteApi::createAnalyzer(inference);
                }
            }();
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
            return made.take();
        }

        srt::SynthUnit unit;
        srt::PackageHandle package;
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
    // The declaration sets a quarter-second beat, so eight notes fit in two seconds, and the first
    // note starts at the start time that the host specified.
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

    // Known notes are stated on the host's timeline, which is also the timeline of the results, and
    // the span begins at four seconds. Reading them as offsets into the span instead would put
    // every one of these four seconds early, and nothing in the result would indicate the error.
    NoteApi::NoteStartInput input;
    input.audio = silence(2.0, 4.0);
    input.knownNotes = {
        {4.0, 0.3 },
        {4.5, 0.7 },
        {5.5, 0.25}
    };

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    BOOST_REQUIRE_EQUAL(result->notes.size(), input.knownNotes.size());
    for (std::size_t i = 0; i < result->notes.size(); ++i) {
        BOOST_CHECK_CLOSE(result->notes[i].start, input.knownNotes[i].start, 1e-9);
        BOOST_CHECK_CLOSE(result->notes[i].duration, input.knownNotes[i].duration, 1e-9);
    }

    // A note before the span is a caller mistake rather than something to clamp.
    NoteApi::NoteStartInput early;
    early.audio = silence(2.0, 4.0);
    early.knownNotes = {
        {0.0, 0.3}
    };
    BOOST_CHECK(!analyzer->start(early));
}

BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_RefusesAudioItWasNotPromised) {
    Loaded loaded("stub-analyzers");
    auto analyzer = loaded.create<F0Api::F0Executive>("f0");

    // Resampling is the responsibility of the host, and an analyzer that resampled silently would
    // change the result without any indication.
    F0Api::F0StartInput wrongRate;
    wrongRate.audio = silence(1.0, 0, 44100);
    BOOST_CHECK(!analyzer->start(wrongRate));

    F0Api::F0StartInput empty;
    empty.audio = silence(0.0);
    BOOST_CHECK(!analyzer->start(empty));

    // maxSegmentDuration is a hard constraint rather than a recommendation: the host slices the
    // audio, and a span longer than the model can encode fails instead of degrading.
    F0Api::F0StartInput tooLong;
    tooLong.audio = silence(61.0);
    BOOST_CHECK(!analyzer->start(tooLong));

    NoteApi::NoteStartInput unknownLanguage;
    unknownLanguage.audio = silence(1.0);
    unknownLanguage.language = "qqq";
    auto note = loaded.create<NoteApi::NoteExecutive>("note");
    BOOST_CHECK(!note->start(unknownLanguage));
}

BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_RefusesAKnobOutsideTheRangeItDeclares) {
    Loaded loaded("stub-analyzers");
    auto analyzer = loaded.create<NoteApi::NoteExecutive>("note");

    // The declaration specifies a range for every knob. A value outside the range is rejected
    // rather than clamped, so that a caller that requests an unsupported setting receives an error
    // instead of a result computed with a different setting. A negative step count previously
    // reached the model as an empty schedule tensor.
    for (const auto &wrong : {-1, 0, 100000}) {
        NoteApi::NoteStartInput input;
        input.audio = silence(1.0);
        input.steps = wrong;
        BOOST_CHECK(!analyzer->start(input));
    }

    NoteApi::NoteStartInput tooLoud;
    tooLoud.audio = silence(1.0);
    tooLoud.notePresenceCutoff = 5.0;
    BOOST_CHECK(!analyzer->start(tooLoud));

    NoteApi::NoteStartInput fine;
    fine.audio = silence(1.0);
    fine.steps = 4;
    fine.notePresenceCutoff = 0.5;
    BOOST_CHECK_MESSAGE(static_cast<bool>(analyzer->start(fine)),
                        "an in-range knob should be accepted");
}

/// A knob that the declaration does not honor has no range to check against, and the declaration
/// excludes the knob from the caller's control, so a supplied value is ignored rather than
/// rejected.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_IgnoresAKnobTheDeclarationDoesNotHonor) {
    Loaded loaded("unknobbed");
    auto analyzer = loaded.create<F0Api::F0Executive>("f0");

    F0Api::F0StartInput input;
    input.audio = silence(1.0);
    input.voicingThreshold = 5.0;
    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    BOOST_CHECK_EQUAL(produced.take()->f0.size(), 100u);
}

BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_RefusesAModelItCannotFeed) {
    // The declaration specifies two channels, and the library cannot widen a span, so the model
    // would receive averaged audio in place of stereo audio. A previous version did so silently:
    // the count was read into a member that no code used.
    Loaded loaded("stereo-model");
    auto analyzer = loaded.create<F0Api::F0Executive>("f0");

    F0Api::F0StartInput input;
    input.audio = silence(1.0);
    auto refused = analyzer->start(input);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().message().find("one channel") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_RefusesAPartialFrame) {
    Loaded loaded("stub-analyzers");
    auto analyzer = loaded.create<F0Api::F0Executive>("f0");

    // Dropping the odd sample would shorten the span by a fraction of a frame and shift nothing
    // else, which appears much later as drift rather than as an error here.
    F0Api::F0StartInput input;
    input.audio = silence(1.0, 0, RATE, 2);
    input.audio.samples.pop_back();
    auto refused = analyzer->start(input);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().message().find("whole number") != std::string::npos);
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
    std::error_code code;

    auto started =
        analyzer->startAsync(input, [&](srt::Expected<std::unique_ptr<F0Api::F0Result>> result) {
            std::lock_guard guard(mutex);
            succeeded = static_cast<bool>(result);
            if (!result) {
                code = result.error().code();
            }
            finished = true;
            done.notify_all();
        });
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(started), otter::test::why(started));

    BOOST_CHECK(analyzer->stop());
    {
        std::unique_lock guard(mutex);
        BOOST_REQUIRE(done.wait_for(guard, std::chrono::seconds(10), [&] { return finished; }));
    }
    // A cancelled execution returns no result. The error code, on which a caller branches, is
    // Cancelled, and the state agrees with the code.
    BOOST_CHECK(!succeeded);
    BOOST_CHECK(code == otter::AnalysisError::Cancelled);
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Canceled);
    BOOST_CHECK(analyzer->waitForFinished());

    // A stop request applies only to the execution that it targets. A flag left set would cancel
    // the next execution, which the caller did not request.
    F0Api::F0StartInput again;
    again.audio = silence(1.0);
    auto after = analyzer->start(again);
    BOOST_CHECK_MESSAGE(static_cast<bool>(after), otter::test::why(after));
}

/// A callback may start the next execution. The worker that delivers the callback then starts
/// its own successor, which is possible only if the worker thread is detached rather than joined.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_ACallbackMayStartTheNextExecution) {
    Loaded loaded("stub-analyzers");
    auto analyzer = loaded.create<F0Api::F0Executive>("f0");
    auto raw = analyzer.get();

    auto input = std::make_shared<F0Api::F0StartInput>();
    input->audio = silence(0.5);

    std::mutex mutex;
    std::condition_variable done;
    int completed = 0;
    bool secondStarted = false;
    bool firstReturned = false;

    auto started =
        raw->startAsync(input, [&](srt::Expected<std::unique_ptr<F0Api::F0Result>> result) {
            {
                std::lock_guard guard(mutex);
                completed += result ? 1 : 0;
            }
            auto again =
                raw->startAsync(input, [&](srt::Expected<std::unique_ptr<F0Api::F0Result>> second) {
                    std::lock_guard guard(mutex);
                    completed += second ? 1 : 0;
                    done.notify_all();
                });
            std::lock_guard guard(mutex);
            secondStarted = static_cast<bool>(again);
            firstReturned = true;
            done.notify_all();
        });
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(started), otter::test::why(started));
    {
        std::unique_lock guard(mutex);
        BOOST_REQUIRE(done.wait_for(guard, std::chrono::seconds(10),
                                    [&] { return firstReturned && completed == 2; }));
        BOOST_CHECK(secondStarted);
    }
    BOOST_CHECK(analyzer->waitForFinished());
}

/// A callback may hold the last reference to the analyzer, so destroying the analyzer inside the
/// callback must work. The worker that delivers the callback then destroys the task on which it
/// runs, which again requires a detached thread rather than a joined thread.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_ACallbackMayDestroyTheAnalyzer) {
    Loaded loaded("stub-analyzers");
    std::shared_ptr<F0Api::F0Executive> owner(loaded.create<F0Api::F0Executive>("f0").release());

    auto input = std::make_shared<F0Api::F0StartInput>();
    input->audio = silence(0.5);

    std::mutex mutex;
    std::condition_variable done;
    bool destroyed = false;

    auto started =
        owner->startAsync(input, [owner, &mutex, &done, &destroyed](
                                     srt::Expected<std::unique_ptr<F0Api::F0Result>>) mutable {
            owner.reset();
            std::lock_guard guard(mutex);
            destroyed = true;
            done.notify_all();
        });
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(started), otter::test::why(started));
    owner.reset();
    std::unique_lock guard(mutex);
    BOOST_REQUIRE(done.wait_for(guard, std::chrono::seconds(10), [&] { return destroyed; }));
}

/// One analyzer serves one span after another, and each result is anchored at the start of its
/// own span; no state accumulates between calls.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_AnchorsEachSegmentWhereItBegan) {
    Loaded loaded("stub-analyzers");
    auto analyzer = loaded.create<F0Api::F0Executive>("f0");

    for (const double begin : {10.0, 11.0, 25.5}) {
        F0Api::F0StartInput input;
        input.audio = silence(1.0, begin);
        auto produced = analyzer->start(input);
        BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
        BOOST_CHECK_CLOSE(produced.take()->startTime, begin, 1e-9);
    }
}

/// Options written for another contract are rejected before any value is read from them.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_RefusesOptionsOfAnotherContract) {
    Loaded loaded("stub-analyzers");
    auto spec = loaded.package.contribution(srt::InferenceCategory::NAME, "f0");
    BOOST_REQUIRE(spec != nullptr);
    auto &inference = *spec->as<srt::InferenceSpec>();

    NoteApi::NoteRuntimeOptions wrong("stub");
    auto made = inference.createInference(F0Api::F0ImportOptions("stub"), wrong);
    BOOST_CHECK(!made);
}

/// Known notes must be ordered and must not overlap, here as in the shipped provider.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_RefusesKnownNotesOutOfOrder) {
    Loaded loaded("stub-analyzers");
    auto analyzer = loaded.create<NoteApi::NoteExecutive>("note");

    NoteApi::NoteStartInput input;
    input.audio = silence(2.0, 4.0);
    input.knownNotes = {
        {5.0, 0.5},
        {4.5, 0.5}
    };
    BOOST_CHECK(!analyzer->start(input));

    NoteApi::NoteStartInput empty;
    empty.audio = silence(2.0, 4.0);
    empty.knownNotes = {
        {4.0, 0.0}
    };
    BOOST_CHECK(!analyzer->start(empty));

    // The span ends at six seconds, and a note may end there but not later.
    NoteApi::NoteStartInput edge;
    edge.audio = silence(2.0, 4.0);
    edge.knownNotes = {
        {5.5, 0.5}
    };
    BOOST_CHECK_MESSAGE(static_cast<bool>(analyzer->start(edge)), "a note may end with the span");

    NoteApi::NoteStartInput late;
    late.audio = silence(2.0, 4.0);
    late.knownNotes = {
        {5.5, 0.75}
    };
    auto refused = analyzer->start(late);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().message().find("ends after") != std::string::npos);
}

/// The task interface without a plugin: a body that throws is an internal fault of the analyzer
/// and is reported with the library's error code rather than as malformed input.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_ABodyThatThrowsIsAnInternalFailure) {
    using Task = otter::AnalysisTask<F0Api::F0StartInput, F0Api::F0Result>;
    Task task([](const F0Api::F0StartInput &) -> srt::Expected<std::unique_ptr<F0Api::F0Result>> {
        throw std::runtime_error("the body gave up");
    });

    F0Api::F0StartInput input;
    auto failed = task.run(input);
    BOOST_REQUIRE(!failed);
    BOOST_CHECK(failed.error().code() == otter::AnalysisError::Internal);
    BOOST_CHECK(failed.error().message().find("the body gave up") != std::string::npos);
    BOOST_CHECK_EQUAL(task.state(), srt::ITask::Failed);
}

/// The state follows the error code of the outcome, not the stop flag. A body that fails for
/// another reason after a stop request arrived reports that failure, and a body that finishes
/// regardless reports its result: in both cases the state describes the outcome that the caller
/// received.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_TheStateFollowsTheErrorCode) {
    using Task = otter::AnalysisTask<F0Api::F0StartInput, F0Api::F0Result>;
    Task *self = nullptr;
    bool fail = true;
    Task task([&](const F0Api::F0StartInput &) -> srt::Expected<std::unique_ptr<F0Api::F0Result>> {
        (void) self->stop();
        if (fail) {
            return srt::Error(srt::Error::InvalidArgument, "rejected for another reason");
        }
        return std::make_unique<F0Api::F0Result>();
    });
    self = &task;

    F0Api::F0StartInput input;
    auto refused = task.run(input);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().code() == srt::Error::InvalidArgument);
    BOOST_CHECK_EQUAL(task.state(), srt::ITask::Failed);

    fail = false;
    auto produced = task.run(input);
    BOOST_CHECK(static_cast<bool>(produced));
    BOOST_CHECK_EQUAL(task.state(), srt::ITask::Succeeded);
}

/// An exception thrown by a callback is caught at the point of delivery. The execution still
/// releases its claim afterwards, so a waiter returns and the next execution is accepted.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_ACallbackThatThrowsReleasesTheExecution) {
    using Task = otter::AnalysisTask<F0Api::F0StartInput, F0Api::F0Result>;
    Task task([](const F0Api::F0StartInput &) -> srt::Expected<std::unique_ptr<F0Api::F0Result>> {
        return std::make_unique<F0Api::F0Result>();
    });

    std::mutex mutex;
    std::condition_variable done;
    bool delivered = false;
    auto started = task.runAsync(std::make_shared<F0Api::F0StartInput>(),
                                 [&](srt::Expected<std::unique_ptr<F0Api::F0Result>>) {
                                     {
                                         std::lock_guard guard(mutex);
                                         delivered = true;
                                     }
                                     done.notify_all();
                                     throw std::runtime_error("the callback gave up");
                                 });
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(started), otter::test::why(started));
    {
        std::unique_lock guard(mutex);
        BOOST_REQUIRE(done.wait_for(guard, std::chrono::seconds(10), [&] { return delivered; }));
    }
    BOOST_CHECK(task.waitForFinished());
    BOOST_CHECK_EQUAL(task.state(), srt::ITask::Succeeded);

    F0Api::F0StartInput again;
    auto after = task.run(again);
    BOOST_CHECK_MESSAGE(static_cast<bool>(after), otter::test::why(after));
}

/// The untyped entry point is the public interface of srt::ITask, so it may receive a payload of
/// any contract. A payload of another contract is rejected before the claim is taken, and the
/// state is left unchanged.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_RefusesAPayloadOfAnotherContract) {
    using Task = otter::AnalysisTask<F0Api::F0StartInput, F0Api::F0Result>;
    bool ran = false;
    Task task([&](const F0Api::F0StartInput &) -> srt::Expected<std::unique_ptr<F0Api::F0Result>> {
        ran = true;
        return std::make_unique<F0Api::F0Result>();
    });
    srt::ITask &untyped = task;

    NoteApi::NoteStartInput wrong;
    auto refused = untyped.start(wrong);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().code() == srt::Error::InvalidArgument);
    BOOST_CHECK(!ran);
    BOOST_CHECK_EQUAL(task.state(), srt::ITask::Idle);

    auto notStarted = untyped.startAsync(std::make_shared<NoteApi::NoteStartInput>(),
                                         [](srt::Expected<std::unique_ptr<srt::TaskResult>>) {});
    BOOST_REQUIRE(!notStarted);
    BOOST_CHECK(notStarted.error().code() == srt::Error::InvalidArgument);
    BOOST_CHECK(task.waitForFinished());
    BOOST_CHECK(!ran);

    F0Api::F0StartInput right;
    BOOST_CHECK(static_cast<bool>(untyped.start(right)));
    BOOST_CHECK(ran);
}

namespace {

    /// A task whose untyped entry point returns a result of another contract, which the typed
    /// interface must reject rather than convert.
    class MismatchedTask : public otter::AnalysisTask<F0Api::F0StartInput, F0Api::F0Result> {
    public:
        MismatchedTask()
            : AnalysisTask([](const F0Api::F0StartInput &)
                               -> srt::Expected<std::unique_ptr<F0Api::F0Result>> {
                  return std::make_unique<F0Api::F0Result>();
              }) {
        }

        srt::Expected<std::unique_ptr<srt::TaskResult>>
            start(const srt::TaskStartInput &) override {
            return std::unique_ptr<srt::TaskResult>(new NoteApi::NoteResult());
        }
    };

}

BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_RefusesAResultOfAnotherContract) {
    MismatchedTask task;
    F0Api::F0StartInput input;
    auto converted = task.run(input);
    BOOST_REQUIRE(!converted);
    BOOST_CHECK(converted.error().code() == otter::AnalysisError::Internal);
}

/// An interpreter may come from a third party, so the executive it creates is checked to belong
/// to the contract rather than assumed to.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_RefusesAnExecutiveOfAnotherContract) {
    Loaded loaded("foreign-executive");
    auto spec = loaded.package.contribution(srt::InferenceCategory::NAME, "f0");
    BOOST_REQUIRE(spec != nullptr);
    auto made = F0Api::createAnalyzer(*spec->as<srt::InferenceSpec>());
    BOOST_REQUIRE(!made);
    BOOST_CHECK(made.error().code() == otter::AnalysisError::Internal);
}

/// A mono span is supplied to the model without a copy of a buffer that can hold minutes of
/// audio; a stereo span is averaged into a separate buffer.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_PassesAMonoSpanWithoutCopying) {
    auto mono = silence(1.0);
    auto prepared = otter::prepareSamples(mono, RATE, 1, 0);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(prepared), otter::test::why(prepared));
    BOOST_CHECK(prepared->data() == mono.samples.data());
    BOOST_CHECK_EQUAL(prepared->size(), mono.samples.size());

    auto stereo = silence(1.0, 0, RATE, 2);
    stereo.samples[0] = 1.0f;
    auto mixed = otter::prepareSamples(stereo, RATE, 1, 0);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(mixed), otter::test::why(mixed));
    BOOST_CHECK(mixed->data() != stereo.samples.data());
    BOOST_CHECK_EQUAL(mixed->size(), stereo.samples.size() / 2);
    BOOST_CHECK_CLOSE(*mixed->begin(), 0.5f, 1e-4);
}

namespace {

    /// A task whose worker thread cannot be started, as when the process has run out of threads.
    class WorkerlessTask : public otter::AnalysisTask<F0Api::F0StartInput, F0Api::F0Result> {
    public:
        WorkerlessTask()
            : AnalysisTask([](const F0Api::F0StartInput &)
                               -> srt::Expected<std::unique_ptr<F0Api::F0Result>> {
                  return std::make_unique<F0Api::F0Result>();
              }) {
        }

    protected:
        void startWorker(std::function<void()>) override {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                    "no thread for the test");
        }
    };

}

/// A failure to start a worker thread is reported as NoWorker, and the claim taken by the start
/// is released, because without a worker no other code would release the claim and every later
/// execution would be rejected.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_ReportsAMissingWorkerAndReleasesTheClaim) {
    WorkerlessTask task;
    bool called = false;
    auto started = task.runAsync(
        std::make_shared<F0Api::F0StartInput>(),
        [&called](srt::Expected<std::unique_ptr<F0Api::F0Result>>) { called = true; });
    BOOST_REQUIRE(!started);
    BOOST_CHECK(started.error().code() == otter::AnalysisError::NoWorker);
    BOOST_CHECK_EQUAL(task.state(), srt::ITask::Failed);
    BOOST_CHECK(task.waitForFinished());
    BOOST_CHECK(!called);

    F0Api::F0StartInput input;
    auto after = task.run(input);
    BOOST_CHECK_MESSAGE(static_cast<bool>(after), otter::test::why(after));
}

/// The language and scheme of an alignment follow the declaration: the default applies if the
/// caller names neither, a language declared with one scheme requires no scheme, and a language
/// declared with several schemes requires the caller to name the scheme.
BOOST_AUTO_TEST_CASE(test_AnalysisRuntime_ChoosesTheDeclaredLanguageAndScheme) {
    Loaded loaded("stub-aligner");
    auto analyzer = loaded.create<AlignApi::AlignExecutive>("align");

    AlignApi::AlignStartInput defaulted;
    defaulted.audio = silence(1.0, 2.0);
    defaulted.lyrics = "a b";
    auto produced = analyzer->start(defaulted);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();
    BOOST_CHECK_EQUAL(result->language, "cmn");
    BOOST_CHECK_EQUAL(result->scheme, "pinyin");
    BOOST_REQUIRE_EQUAL(result->words.size(), 2u);
    BOOST_CHECK_CLOSE(result->words.front().start, 2.0, 1e-9);
    BOOST_CHECK_CLOSE(result->words.back().start + result->words.back().duration, 3.0, 1e-9);

    AlignApi::AlignStartInput ambiguous;
    ambiguous.audio = silence(1.0);
    ambiguous.lyrics = "aa";
    ambiguous.language = "yue";
    BOOST_CHECK(!analyzer->start(ambiguous));

    AlignApi::AlignStartInput named;
    named.audio = silence(1.0);
    named.lyrics = "aa";
    named.language = "yue";
    named.scheme = "yale";
    auto yale = analyzer->start(named);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(yale), otter::test::why(yale));
    BOOST_CHECK_EQUAL(yale.take()->scheme, "yale");

    AlignApi::AlignStartInput undeclared;
    undeclared.audio = silence(1.0);
    undeclared.lyrics = "a";
    undeclared.language = "cmn";
    undeclared.scheme = "zhuyin";
    BOOST_CHECK(!analyzer->start(undeclared));
}

BOOST_AUTO_TEST_SUITE_END()
