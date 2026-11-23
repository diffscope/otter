// The game provider against real ONNX graphs.
//
// The five models have fixture weights and compute arithmetic that a test can predict. The tests
// verify the provider's side of the contract: every declared input reaches its model, the four
// stages are chained in the correct order, the knobs reach the models, the notes are placed on the
// host's timeline in seconds, and the alignment path runs the fifth model instead of passing
// zeros.

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/SVS/InferenceContrib.h>
#include <synthrt/SVS/InferenceInterpreterPlugin.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Api/Note/1/NoteApiL1.h>

#include <onnxutil/OnnxSupport.h>
#include <game/GameInterpreter.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

#include "TestSupport.h"

namespace NoteApi = otter::Api::Note::L1;
namespace CommonApi = otter::Api::Common::L1;

namespace {

    // The plugin that main.cpp of game declares, around the interpreter that this test
    // links. The test registers it as a static plugin with the plugin.json of game, so that
    // discovery selects this class rather than the library in the plugin tree.
    class GamePlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(const srt::ContribContractTriplet &triplet) override {
            if (auto served = otter::onnx::checkServed(
                    triplet, srt::ContribContractTriplet(NoteApi::Contract.interfaceId(),
                                                         NoteApi::Contract.level(),
                                                         otter::game::Triplet.variant()));
                !served) {
                return served.takeError();
            }
            return std::unique_ptr<srt::ContribInterpreter>(new otter::game::GameInterpreter());
        }
    };

}

STDC_EXPORT_STATIC_PLUGIN(GamePlugin, srt::InferenceInterpreterPlugin::IID,
                          otter::test::pluginManifest())

namespace {

    constexpr int RATE = 44100;

    // The models compute in float32, so a duration of a fifth of a second is returned as
    // 0.19999998. BOOST_CHECK_CLOSE takes a percentage, and this value corresponds to float
    // precision.
    constexpr double TOLERANCE = 1e-4;

    using FixturesOrSkip = otter::test::FixturesOrSkip;

    struct Host : otter::test::ModelHost {
        srt::ContribSpec *load() {
            return ModelHost::load("fixture-note", "note");
        }

        std::unique_ptr<NoteApi::Executive> open() {
            auto spec = load();
            auto made = NoteApi::Executive::create(*spec->as<srt::InferenceSpec>());
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
            return made.take();
        }
    };

    // A host that loads a package whose graphs include the one that converts durations into
    // boundaries, although its exports promise no alignment on known notes. The variant accepts
    // such a declaration, because that model is required only of a declaration that promises it.
    struct HostWithoutKnownNotes : otter::test::ModelHost {
        std::unique_ptr<NoteApi::Executive> open() {
            auto spec = ModelHost::load("fixture-note-no-known-notes", "note");
            auto made = NoteApi::Executive::create(*spec->as<srt::InferenceSpec>());
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
            return made.take();
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

BOOST_TEST_GLOBAL_FIXTURE(FixturesOrSkip);

BOOST_AUTO_TEST_SUITE(test_GameInterpreter)

BOOST_AUTO_TEST_CASE(test_GameInterpreter_ReportsWhatTheDeclarationExports) {
    Host host;
    auto spec = host.load();
    auto schema = spec->exports()->as<NoteApi::Exports>();
    BOOST_REQUIRE(schema != nullptr);

    // The exports are the declaration's values, read through the contract's reader, and the
    // provider has checked them against the configuration before the package could load.
    BOOST_CHECK_EQUAL(schema->sampleRate, RATE);
    BOOST_CHECK_CLOSE(schema->maxSegmentDuration, 60.0, 1e-9);
    BOOST_CHECK(schema->supportsKnownNotes);
    BOOST_REQUIRE_EQUAL(schema->languages.size(), 2u);
    BOOST_CHECK(schema->steps.honored);
    BOOST_CHECK_EQUAL(schema->steps.defaultValue, 8);
    BOOST_CHECK_CLOSE(schema->boundaryRadius.defaultValue, 0.2, 1e-9);
}

BOOST_AUTO_TEST_CASE(test_GameInterpreter_ChainsTheStagesAndPlacesNotesInSeconds) {
    Host host;
    auto analyzer = host.open();

    NoteApi::StartInput input;
    input.audio = tone(2.0, 7.5);
    input.notePresenceCutoff = 0.0;
    std::vector<double> reported;
    input.progress = [&reported](double value) {
        reported.push_back(value);
        return true;
    };

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    // Two seconds at a 10 ms frame is 200 frames. The declaration's 0.2 s radius makes a boundary
    // every 20 frames, which yields ten notes of 0.2 s each. This count is reached only if all four
    // stages ran and were chained in the correct order.
    BOOST_REQUIRE_EQUAL(result->notes.size(), 10u);
    BOOST_CHECK_CLOSE(result->notes.front().start, 7.5, TOLERANCE);
    BOOST_CHECK_CLOSE(result->notes.front().duration, 0.2, TOLERANCE);

    // The notes are contiguous, ordered, and placed in absolute seconds on the host's timeline.
    for (std::size_t i = 1; i < result->notes.size(); ++i) {
        BOOST_CHECK_CLOSE(result->notes[i].start,
                          result->notes[i - 1].start + result->notes[i - 1].duration, TOLERANCE);
    }
    BOOST_CHECK_CLOSE(result->notes.back().start + result->notes.back().duration, 9.5, TOLERANCE);

    // The fixture estimator returns a chromatic run from middle C, so the note order is observable.
    BOOST_CHECK_EQUAL(result->notes.front().key, 60);
    BOOST_CHECK_EQUAL(result->notes[1].key, 61);

    BOOST_REQUIRE(!reported.empty());
    BOOST_CHECK_CLOSE(reported.back(), 1.0, 1e-9);
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}

// The release package, when the host fetched one, runs the trained graphs. The fixture cases pin
// the arithmetic of the provider. This one proves that the weights load and that a transcript
// arrives on the caller's timeline.
BOOST_AUTO_TEST_CASE(test_GameInterpreter_TranscribesWithTheTrainedModel) {
    Host host;
    auto spec = host.loadDirectory(otter::test::realModelsRoot() / "otter-game", "note");
    auto made = NoteApi::Executive::create(*spec->as<srt::InferenceSpec>());
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
    auto analyzer = made.take();

    NoteApi::StartInput input;
    input.audio = tone(2.0, 1.5);
    // The cutoff of the declaration drops frames the model is unsure about. A synthetic tone is
    // not what the model was trained on, so the test asks it to keep every frame it produced.
    input.notePresenceCutoff = 0.0;

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    // A trained estimator answers with what it heard rather than a fixed schedule, so only the
    // properties that hold for any transcript are asserted.
    BOOST_REQUIRE(!result->notes.empty());
    for (std::size_t i = 0; i < result->notes.size(); ++i) {
        const auto &note = result->notes[i];
        BOOST_CHECK_GE(note.start, 1.5 - TOLERANCE);
        BOOST_CHECK_LE(note.start + note.duration, 3.5 + TOLERANCE);
        BOOST_CHECK_GT(note.duration, 0.0);
        BOOST_CHECK_GE(note.key, 0);
        BOOST_CHECK_LE(note.key, 127);
        if (i > 0) {
            BOOST_CHECK_GE(note.start, result->notes[i - 1].start);
        }
    }
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}
BOOST_AUTO_TEST_CASE(test_GameInterpreter_SendsTheKnobsToTheModels) {
    Host host;
    auto analyzer = host.open();

    const auto count = [&analyzer](double radius, double cutoff) {
        NoteApi::StartInput input;
        input.audio = tone(2.0);
        input.boundaryRadius = radius;
        input.notePresenceCutoff = cutoff;
        auto produced = analyzer->start(input);
        BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
        return produced.take()->notes.size();
    };

    // A narrower radius yields more boundaries and therefore more notes, which shows that the knob
    // reached the segmenter.
    BOOST_CHECK_GT(count(0.1, 0.0), count(0.4, 0.0));
    // The cutoff filters the estimator's output, and the fixture alternates confidences, so
    // raising the cutoff above the lower confidence halves the count.
    BOOST_CHECK_LT(count(0.2, 0.6), count(0.2, 0.0));
}

BOOST_AUTO_TEST_CASE(test_GameInterpreter_AlignsAgainstTheNotesTheCallerKnows) {
    Host host;
    auto analyzer = host.open();

    // The span does not begin at zero, because the time base matters only in that case: if the
    // notes were read as offsets into the span, they would be placed after a thirty-second gap,
    // and every boundary would fall outside the available frames.
    constexpr double SPAN_START = 30.0;

    NoteApi::StartInput free;
    free.audio = tone(2.0, SPAN_START);
    free.boundaryRadius = 0.4;
    free.notePresenceCutoff = 0.0;
    auto without = analyzer->start(free);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(without), otter::test::why(without));
    const auto unconditioned = without.take()->notes.size();

    // The same audio is analyzed with known notes. The fifth model converts their durations into
    // boundaries that the segmenter keeps, so the result must change. The result could not change
    // while the segmenter received zeros, as in the original implementation.
    NoteApi::StartInput aligned;
    aligned.audio = tone(2.0, SPAN_START);
    aligned.boundaryRadius = 0.4;
    aligned.notePresenceCutoff = 0.0;
    aligned.knownNotes = {
        {SPAN_START + 0.0, 0.1},
        {SPAN_START + 0.1, 0.1},
        {SPAN_START + 0.2, 0.1},
        {SPAN_START + 0.3, 0.1},
        {SPAN_START + 0.4, 0.1}
    };
    auto with = analyzer->start(aligned);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(with), otter::test::why(with));
    auto conditionedResult = with.take();

    BOOST_CHECK_GT(conditionedResult->notes.size(), unconditioned);
    // The notes still lie inside the span named by the host.
    for (const auto &note : conditionedResult->notes) {
        BOOST_CHECK_GE(note.start, SPAN_START);
        BOOST_CHECK_LE(note.start, SPAN_START + 2.0);
    }
}

BOOST_AUTO_TEST_CASE(test_GameInterpreter_RefusesWhatItCannotHonor) {
    Host host;
    auto analyzer = host.open();

    NoteApi::StartInput wrongRate;
    wrongRate.audio = tone(1.0);
    wrongRate.audio.sampleRate = 16000;
    auto refusedRate = analyzer->start(wrongRate);
    BOOST_REQUIRE(!refusedRate);
    BOOST_CHECK(refusedRate.error().message().find("44100") != std::string::npos);

    NoteApi::StartInput unknownLanguage;
    unknownLanguage.audio = tone(1.0);
    unknownLanguage.language = "qqq";
    BOOST_CHECK(!analyzer->start(unknownLanguage));

    // Known notes out of order would place every later note early instead of causing a failure,
    // so they are rejected.
    NoteApi::StartInput crossed;
    crossed.audio = tone(1.0);
    crossed.knownNotes = {
        {0.5, 0.2},
        {0.1, 0.2}
    };
    BOOST_CHECK(!analyzer->start(crossed));

    // Known notes are on the host's timeline, so a known note before the start of the span is
    // invalid.
    NoteApi::StartInput early;
    early.audio = tone(1.0, 5.0);
    early.knownNotes = {
        {0.0, 0.2}
    };
    BOOST_CHECK(!analyzer->start(early));

    // A known note that runs past the end of the span would be converted into boundaries the
    // frames do not have.
    NoteApi::StartInput late;
    late.audio = tone(1.0, 5.0);
    late.knownNotes = {
        {5.5, 0.4},
        {5.9, 0.2}
    };
    auto refusedLate = analyzer->start(late);
    BOOST_REQUIRE(!refusedLate);
    BOOST_CHECK(refusedLate.error().message().find("ends after") != std::string::npos);

    // The configuration numbers eng, but the exports do not declare it, so the package does not
    // offer it.
    NoteApi::StartInput undeclared;
    undeclared.audio = tone(1.0);
    undeclared.language = "eng";
    auto refusedLanguage = analyzer->start(undeclared);
    BOOST_REQUIRE(!refusedLanguage);
    BOOST_CHECK(refusedLanguage.error().code() == srt::Error::InvalidArgument);
    // One sentence: what the caller asked for, then the capability this model lacks. The stub
    // words its own rejection of an undeclared language the same way.
    BOOST_CHECK(refusedLanguage.error().message().find("the caller asked for the language eng, "
                                                       "which this model cannot transcribe") !=
                std::string::npos);

    // A knob outside the range that the declaration reports is rejected.
    NoteApi::StartInput wrongKnob;
    wrongKnob.audio = tone(1.0);
    wrongKnob.steps = 0;
    BOOST_CHECK(!analyzer->start(wrongKnob));
}

// A declaration that promises no alignment on known notes loads, because the model that converts
// durations into boundaries is required only of a declaration that promises it. An execution that
// receives known notes is then refused, rather than having them ignored, and the message names
// the missing capability. This package carries that model as well, so only the declaration can
// refuse these notes: a build that dropped that half of the check would accept them. The same
// package transcribes a span without them, which is what tells this refusal apart from a package
// that failed to load.
BOOST_AUTO_TEST_CASE(test_GameInterpreter_RefusesKnownNotesItDidNotPromise) {
    HostWithoutKnownNotes host;
    auto analyzer = host.open();

    NoteApi::StartInput known;
    known.audio = tone(1.0);
    known.knownNotes = {
        {0.0, 0.2},
        {0.5, 0.2}
    };
    auto refused = analyzer->start(known);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().code() == srt::Error::InvalidArgument);
    BOOST_CHECK(refused.error().message().find("this model cannot be conditioned on known notes") !=
                std::string::npos);

    // The same analyzer transcribes a span for which the caller supplies no note. The knobs are
    // the ones this variant falls back to, since the declaration promises none, so the number of
    // notes it finds is not asserted here.
    NoteApi::StartInput free;
    free.audio = tone(1.0);
    auto transcribed = analyzer->start(free);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(transcribed), otter::test::why(transcribed));
}

// The two blocks of a declaration must agree. A language that the exports declare but the model
// cannot number, or an alignment path declared without the model that performs it, is rejected
// at load time, where both blocks are available, instead of being discovered by a failing
// execution.
BOOST_AUTO_TEST_CASE(test_GameInterpreter_RefusesALanguageTheModelCannotNumber) {
    Host host;
    BOOST_CHECK(host.refusal("fixture-note-unnumbered").find("numbering") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(test_GameInterpreter_RefusesAnAlignmentPromiseWithoutTheModel) {
    Host host;
    BOOST_CHECK(host.refusal("fixture-note-no-alignment").find("durationToBoundary") !=
                std::string::npos);
}

// A host may request a transcription without specifying a language, in which case the declared
// default applies. Exports that list languages without a default leave that case undefined, and
// the execution would otherwise run in the language the model numbers zero, which is not a neutral
// choice and, in the packages that ship this variant, not a declared language.
BOOST_AUTO_TEST_CASE(test_GameInterpreter_RefusesNumberedLanguagesWithoutADefault) {
    Host host;
    BOOST_CHECK(host.refusal("fixture-note-no-default").find("defaultLanguage") !=
                std::string::npos);
}

// The five models take one channel each, and the variant mixes nothing down: a declaration of two
// describes audio that a host prepares and the provider would silently reduce before the encoder
// saw it. The count is checked where the declaration is read, and no value of the models
// contradicts it, which is why the package carries the same four models as the ones above.
BOOST_AUTO_TEST_CASE(test_GameInterpreter_RefusesADeclarationOfTwoChannels) {
    Host host;
    const auto refusal = host.refusalError("fixture-note-two-channels");
    BOOST_CHECK(refusal.code() == srt::Error::InvalidFormat);
    BOOST_CHECK(refusal.message().find("the game variant feeds its models one channel") !=
                std::string::npos);
    BOOST_CHECK(refusal.message().find("declare 2") != std::string::npos);
}

// A stop requested between two models makes the execution report Cancelled and no result, and it
// does not carry over to the next execution. The stop is requested from the progress callback,
// which the provider calls on the executing thread after the encoder, so the timing is not left
// to a race with models that take milliseconds.
BOOST_AUTO_TEST_CASE(test_GameInterpreter_ReportsACancellation) {
    Host host;
    auto analyzer = host.open();

    // The provider's progress milestones are report(0) before the encoder, report(0.3) after it,
    // and report(0.8) after the boundaries become durations (game/GameInterpreter.cpp). The
    // callback below stops the execution only where a reported value matches, so renaming a
    // milestone there would let the execution finish and fail this case as a cancellation that did
    // not happen.
    for (const double at : {0.0, 0.3, 0.8}) {
        NoteApi::StartInput input;
        input.audio = tone(1.0);
        input.progress = [&analyzer, at](double value) {
            if (value == at) {
                (void) analyzer->stop();
            }
            return true;
        };
        auto stopped = analyzer->start(input);
        BOOST_REQUIRE(!stopped);
        BOOST_CHECK(stopped.error().code() == otter::AnalysisError::Cancelled);
        BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Canceled);
    }

    NoteApi::StartInput again;
    again.audio = tone(1.0);
    auto produced = analyzer->start(again);
    BOOST_CHECK_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}

BOOST_AUTO_TEST_SUITE_END()
