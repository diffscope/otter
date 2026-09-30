// The hfa provider against a real ONNX graph.
//
// The graph emits a fixed schedule: one second of audio consists of five spans of twenty frames,
// one span per phoneme of the expansion of the lyrics "a b", with the edge head marking the four
// frames at which the spans meet and the non-speech head marking a breath inside the middle
// separator. The expected result is therefore known before the model runs, and the tests verify
// the provider's side of the work: the lyrics reach the dictionary and become the phoneme
// sequence, the model receives the span named by the host, the decode's output is placed on the
// host's timeline in seconds, the knobs reach the decode, and the declaration is checked against
// the models it names.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <synthrt/SVS/InferenceContrib.h>
#include <synthrt/Support/JSON.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Api/Align/1/AlignApiL1.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

#include "TestSupport.h"

namespace fs = std::filesystem;
namespace AlignApi = otter::Api::Align::L1;
namespace CommonApi = otter::Api::Common::L1;

namespace {

    constexpr int RATE = 44100;

    /// The schedule that the fixture graph emits for one second of audio, derived from the
    /// parameters of the graph, which the fixture script writes next to the graph as
    /// schedule.json.
    ///
    /// The frames are cut into equal spans -- silence, "a", a separator with a breath inside it,
    /// "b", silence -- and every time below is a whole number of frames. A boundary that the
    /// fixture places at a frame index is returned at the time of that frame, so every boundary is
    /// asserted with a tolerance of one frame instead of one sample.
    struct Schedule {
        /// One frame, in seconds.
        double frame = 0;

        /// One span, in seconds: the length of each word and of the separator.
        double span = 0;

        /// Where the breath starts, in seconds from the start of the audio.
        double breathStart = 0;

        /// The frames the breath covers.
        int breathFrames = 0;

        /// The probability that the non-speech head assigns to the breath.
        double breathProbability = 0;
    };

    Schedule readSchedule() {
        const auto path = fs::path(OTTER_TEST_FIXTURE_DIR) / "fixture-align" / "schedule.json";
        std::ifstream file(path);
        BOOST_REQUIRE_MESSAGE(file.is_open(), "the fixture should carry schedule.json");
        const std::string text((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
        stdc::json::ParseError problem;
        const auto value = srt::JsonValue::fromJson(text, false, &problem);
        BOOST_REQUIRE(!problem);
        const auto object = value.toObject();
        const auto number = [&object](const char *key) { return object.at(key).toDouble(); };
        const auto whole = [&object](const char *key) {
            return static_cast<int>(object.at(key).toInt());
        };

        // Every case feeds one second of audio, and the graph counts the whole hops in it.
        const int frames = whole("sampleRate") / whole("hopSize");
        const int span = frames / whole("spans");
        Schedule schedule;
        schedule.frame = static_cast<double>(whole("hopSize")) / whole("sampleRate");
        schedule.span = span * schedule.frame;
        schedule.breathStart = (2 * span + whole("breathStartOffset")) * schedule.frame;
        schedule.breathFrames = span - whole("breathStartOffset") - whole("breathEndOffset");
        schedule.breathProbability = number("breathProbability");
        return schedule;
    }

    using FixturesOrSkip = otter::test::FixturesOrSkip;

    struct Host : otter::test::ModelHost {
        srt::ContribSpec *load(const std::string &name = "fixture-align") {
            return ModelHost::load(name, "align");
        }

        std::unique_ptr<AlignApi::AlignExecutive> open() {
            auto spec = load();
            auto made = AlignApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
            return made.take();
        }
    };

    /// Returns silent audio of \a seconds starting at \a startTime. One second corresponds to a
    /// hundred frames of the fixture schedule, five spans of twenty. Silence suffices because the
    /// fixture graph ignores the sample values.
    CommonApi::AudioSegment tone(double seconds = 1.0, double startTime = 0) {
        CommonApi::AudioSegment audio;
        audio.sampleRate = RATE;
        audio.channelCount = 1;
        audio.samples.assign(static_cast<std::size_t>(seconds * RATE), 0.0f);
        audio.startTime = startTime;
        return audio;
    }

    /// Fills \a input with one second of audio and the lyrics \a text. The function writes into
    /// the caller's input because a start input is not copyable: the contract passes the caller's
    /// own object to the provider.
    void lyrics(AlignApi::AlignStartInput &input, const std::string &text, double startTime = 0) {
        input.audio = tone(1.0, startTime);
        input.lyrics = text;
    }

    std::vector<std::string> texts(const AlignApi::AlignResult &result) {
        std::vector<std::string> out;
        for (const auto &word : result.words) {
            out.push_back(word.text);
        }
        return out;
    }

}

BOOST_TEST_GLOBAL_FIXTURE(FixturesOrSkip);

BOOST_AUTO_TEST_SUITE(test_Hfa)

BOOST_AUTO_TEST_CASE(test_Hfa_ReportsWhatTheDeclarationExports) {
    Host host;
    auto spec = host.load();
    auto schema = spec->exports()->as<AlignApi::AlignSchema>();
    BOOST_REQUIRE(schema != nullptr);

    // The exports are the declaration's values, read through the contract's reader, and the
    // provider has checked them against the model's config.json and vocab.json before the package
    // could load.
    BOOST_CHECK_EQUAL(schema->sampleRate, RATE);
    BOOST_CHECK_EQUAL(schema->channelCount, 1);
    BOOST_CHECK_CLOSE(schema->maxSegmentDuration, 60.0, 1e-9);
    // A language is declared with the scheme of its phonemes, the form of its lyrics, and the
    // phonemes a result can contain.
    BOOST_REQUIRE_EQUAL(schema->languages.size(), 1u);
    const auto &language = schema->languages.front();
    BOOST_CHECK_EQUAL(language.language, "cmn");
    BOOST_CHECK_EQUAL(language.scheme, "pinyin");
    BOOST_CHECK(language.lyrics == AlignApi::LyricsForm::Scheme);
    BOOST_CHECK(language.phonemes == std::vector<std::string>({"a", "b"}));
    BOOST_CHECK_EQUAL(schema->defaultLanguage, "cmn");
    BOOST_CHECK(schema->defaultNonSpeechPhonemes == std::vector<std::string>({"AP"}));
    BOOST_REQUIRE_EQUAL(schema->nonSpeechPhonemes.size(), 2u);
    BOOST_CHECK_EQUAL(schema->nonSpeechPhonemes.front(), "AP");
    BOOST_CHECK_EQUAL(schema->silenceLabel, "SP");
    BOOST_CHECK(schema->nonSpeechThreshold.honored);
    BOOST_CHECK_CLOSE(schema->nonSpeechThreshold.defaultValue, 0.5, 1e-9);
    BOOST_CHECK(schema->nonSpeechMinDuration.honored);
    BOOST_CHECK_CLOSE(schema->nonSpeechMinDuration.defaultValue, 0.1, 1e-9);
    BOOST_CHECK(schema->gapFill.honored);
    BOOST_CHECK_CLOSE(schema->gapFill.defaultValue, 0.1, 1e-9);
}

BOOST_AUTO_TEST_CASE(test_Hfa_PlacesTheWordsTheLyricsName) {
    Host host;
    auto analyzer = host.open();
    const auto schedule = readSchedule();

    // The span does not begin at zero, because the time base matters only in that case: if the
    // times were read as offsets, every boundary below would be thirty seconds early.
    constexpr double SPAN_START = 30.0;

    AlignApi::AlignStartInput input;
    lyrics(input, "a b", SPAN_START);
    std::vector<double> reported;
    input.progress = [&reported](double value) { reported.push_back(value); };

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    // The result records the language and scheme of its phonemes, because the caller may have
    // left both to the declared defaults.
    BOOST_CHECK_EQUAL(result->language, "cmn");
    BOOST_CHECK_EQUAL(result->scheme, "pinyin");

    // The fixture's schedule is silence, "a", a separator with a breath in it, "b", silence. The
    // two words come from the lyrics; the labels around and between them are inserted by the
    // provider, so that a host can identify the parts that were not sung.
    const std::vector<std::string> expected = {"SP", "a", "AP", "b", "SP"};
    BOOST_CHECK(texts(*result) == expected);
    BOOST_REQUIRE_EQUAL(result->words.size(), expected.size());

    // The words cover the span: they begin at the start time that the host states for the audio
    // and end at the end of the audio, without gaps between them or at either end.
    BOOST_CHECK_CLOSE(result->words.front().start, SPAN_START, 1e-6);
    BOOST_CHECK_CLOSE(result->words.back().start + result->words.back().duration,
                      SPAN_START + 1.0, 1e-6);
    for (std::size_t i = 1; i < result->words.size(); ++i) {
        const auto &previous = result->words[i - 1];
        BOOST_CHECK_SMALL(result->words[i].start - (previous.start + previous.duration), 1e-6);
        BOOST_CHECK_GT(result->words[i].duration, 0.0);
    }

    // "a" owns the second span and "b" the fourth, and each is one phoneme wide: the dictionary's
    // two entries are one phoneme each, and the model's language prefix is not part of the word
    // written by the caller.
    //
    // The boundaries lie half a frame before the frame that the fixture marked, because the decode
    // uses the edge curve to locate a transition inside a frame. A boundary therefore matches the
    // frame grid only to within half a frame, and the assertions use this resolution.
    const auto &first = result->words[1];
    const auto &second = result->words[3];
    BOOST_CHECK_SMALL((first.start - SPAN_START) - schedule.span, schedule.frame);
    BOOST_CHECK_SMALL((second.start - SPAN_START) - 3 * schedule.span, schedule.frame);
    BOOST_REQUIRE_EQUAL(first.phones.size(), 1u);
    BOOST_CHECK_EQUAL(first.phones.front().text, "a");
    BOOST_REQUIRE_EQUAL(second.phones.size(), 1u);
    BOOST_CHECK_EQUAL(second.phones.front().text, "b");

    // The breath is inside the separator, and the separator is wider than the gap fill, so the
    // caller receives three spans instead of one: silence, breath, silence.
    const auto &breath = result->words[2];
    BOOST_CHECK_GE(breath.start, first.start + first.duration);
    BOOST_CHECK_LE(breath.start + breath.duration, second.start);
    BOOST_CHECK_GT(breath.duration, 0.1);
    BOOST_CHECK_SMALL(breath.start - SPAN_START - schedule.breathStart, schedule.frame);
    // The classifier reports the fixture's breath frames minus one frame, and the gap fill then
    // extends the breath to the following word, so the breath covers the detected frames plus the
    // pause after them.
    BOOST_CHECK_GE(breath.duration, (schedule.breathFrames - 1) * schedule.frame - schedule.frame);
    BOOST_CHECK_SMALL(breath.start + breath.duration - second.start, 1e-9);

    // A gap short enough to be a pause within the words is absorbed into them, and the word's
    // phone is extended with the word, so the phone still covers its entire word.
    BOOST_CHECK_SMALL(first.start + first.duration - breath.start, 1e-9);
    BOOST_CHECK_CLOSE(first.phones.front().start, first.start, 1e-9);
    BOOST_CHECK_SMALL(first.phones.front().duration - first.duration, 1e-9);
    // "b" is followed by more than a gap fill of silence, so it keeps the span that the model
    // assigned to it.
    BOOST_CHECK_SMALL(second.duration - schedule.span, schedule.frame);

    BOOST_REQUIRE(!reported.empty());
    BOOST_CHECK_CLOSE(reported.back(), 1.0, 1e-9);
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}

BOOST_AUTO_TEST_CASE(test_Hfa_SendsTheKnobsToTheDecoder) {
    Host host;
    auto analyzer = host.open();
    const auto schedule = readSchedule();

    const auto run = [&analyzer](const AlignApi::AlignStartInput &input) {
        auto produced = analyzer->start(input);
        BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
        return produced.take();
    };
    const auto breaths = [](const AlignApi::AlignResult &result) {
        std::size_t count = 0;
        for (const auto &word : result.words) {
            if (word.text == "AP") {
                ++count;
            }
        }
        return count;
    };

    // With the defaults the separator is wider than the gap fill, so it stays three spans and the
    // breath is reported.
    AlignApi::AlignStartInput defaults;
    lyrics(defaults, "a b");
    auto plain = run(defaults);
    BOOST_CHECK_EQUAL(plain->words.size(), 5u);
    BOOST_CHECK_EQUAL(breaths(*plain), 1u);

    // Turning the gap fill off leaves the separator and both edges of the span as silence, since
    // neither the leading nor the trailing silence is absorbed either.
    AlignApi::AlignStartInput unfilled;
    lyrics(unfilled, "a b");
    unfilled.gapFill = 0.0;
    BOOST_CHECK(texts(*run(unfilled)) ==
                (std::vector<std::string>{"SP", "a", "SP", "AP", "SP", "b", "SP"}));

    // With the defaults the separator is wider than the declared gap fill, and the cases below
    // move the gap fill across the separator width.
    BOOST_REQUIRE_GT(schedule.span, 0.1);

    // Raising it past the separator absorbs the separator into the words around it, and the
    // leading and trailing silence with it, so no silence remains.
    AlignApi::AlignStartInput filled;
    lyrics(filled, "a b");
    filled.gapFill = schedule.span + 0.1;
    auto absorbed = run(filled);
    BOOST_CHECK(texts(*absorbed) == (std::vector<std::string>{"a", "AP", "b"}));
    BOOST_CHECK_CLOSE(absorbed->words.front().start, 0.0, 1e-6);
    BOOST_CHECK_SMALL(absorbed->words.front().duration - schedule.breathStart, schedule.frame);
    BOOST_CHECK_CLOSE(absorbed->words.back().start + absorbed->words.back().duration, 1.0, 1e-6);

    // A breath shorter than the minimum duration is not reported at all, neither as a zero-length
    // span nor as silence, and the separator becomes one span of silence.
    AlignApi::AlignStartInput minDuration;
    lyrics(minDuration, "a b");
    BOOST_REQUIRE_GT(schedule.span, schedule.breathFrames * schedule.frame);
    minDuration.nonSpeechMinDuration = schedule.span;
    auto longer = run(minDuration);
    BOOST_CHECK(texts(*longer) == (std::vector<std::string>{"SP", "a", "SP", "b", "SP"}));
    BOOST_CHECK_EQUAL(breaths(*longer), 0u);

    // The fixture's breath lies above the declared default threshold, and this threshold lies
    // above the breath, so the result changes if the threshold reaches the classifier.
    BOOST_REQUIRE_GT(schedule.breathProbability, 0.5);
    AlignApi::AlignStartInput threshold;
    lyrics(threshold, "a b");
    threshold.nonSpeechThreshold = (schedule.breathProbability + 1.0) / 2;
    BOOST_CHECK_EQUAL(breaths(*run(threshold)), 0u);
}

BOOST_AUTO_TEST_CASE(test_Hfa_RefusesWhatItCannotHonor) {
    Host host;
    auto analyzer = host.open();

    AlignApi::AlignStartInput wrongRate;
    lyrics(wrongRate, "a b");
    wrongRate.audio.sampleRate = 16000;
    auto refusedRate = analyzer->start(wrongRate);
    BOOST_REQUIRE(!refusedRate);
    BOOST_CHECK(refusedRate.error().message().find("44100") != std::string::npos);

    // An alignment without lyrics has nothing to align, and returning a transcription instead
    // would implement a different contract.
    AlignApi::AlignStartInput noLyrics;
    lyrics(noLyrics, "");
    auto empty = analyzer->start(noLyrics);
    BOOST_REQUIRE(!empty);
    BOOST_CHECK(empty.error().message().find("lyrics") != std::string::npos);

    AlignApi::AlignStartInput unknownLanguage;
    lyrics(unknownLanguage, "a b");
    unknownLanguage.language = "qqq";
    BOOST_CHECK(!analyzer->start(unknownLanguage));

    // A scheme not declared for the language is rejected rather than ignored, because the
    // returned phonemes would otherwise be in a notation other than the requested one.
    AlignApi::AlignStartInput unknownScheme;
    lyrics(unknownScheme, "a b");
    unknownScheme.scheme = "jyutping";
    auto wrongScheme = analyzer->start(unknownScheme);
    BOOST_REQUIRE(!wrongScheme);
    BOOST_CHECK(wrongScheme.error().message().find("jyutping") != std::string::npos);

    // A label that the module does not declare is rejected instead of ignored, because silence in
    // its place would be indistinguishable from "none found".
    AlignApi::AlignStartInput unknownLabel;
    lyrics(unknownLabel, "a b");
    unknownLabel.nonSpeechPhonemes = {"XX"};
    BOOST_CHECK(!analyzer->start(unknownLabel));

    // Lyrics of which no word is in the dictionary would yield a result consisting only of
    // silence, so they are rejected before the model runs.
    AlignApi::AlignStartInput unknownLyrics;
    lyrics(unknownLyrics, "zzz");
    BOOST_CHECK(!analyzer->start(unknownLyrics));

    // A knob outside the range that the declaration reports is rejected.
    AlignApi::AlignStartInput wrongKnob;
    lyrics(wrongKnob, "a b");
    wrongKnob.gapFill = 5.0;
    BOOST_CHECK(!analyzer->start(wrongKnob));

    // Stopping an aligner that is not running is not an error: a host takes this path when it
    // destroys an analyzer that it has already waited for.
    BOOST_CHECK(analyzer->stop());
    BOOST_CHECK(analyzer->waitForFinished());
    BOOST_CHECK_NE(analyzer->state(), srt::ITask::Running);
}

/// A declaration must agree with the models it names. Each of these packages contains one model
/// property that the declaration contradicts. The provider reads the model's own files at load
/// time, so the rejection occurs where the declaration and the model's files are both available
/// instead of after a run whose wrong result cannot be detected.
BOOST_AUTO_TEST_CASE(test_Hfa_RefusesDeclarationsItsModelCannotHonor) {
    Host host;

    const auto refused = [&host](const char *name) { return host.refusal(name); };

    // The models were trained at one rate, and a host preparing audio at another does not fail:
    // it aligns at the wrong speed, which yields a plausible but wrong result. The message names
    // the rate at which the model runs.
    BOOST_CHECK(refused("fixture-align-wrong-rate").find("16000") != std::string::npos);

    // A language with no dictionary cannot be expanded into phonemes at all.
    BOOST_CHECK(refused("fixture-align-no-dictionary").find("no dictionary") != std::string::npos);

    // A non-speech label absent from the vocabulary would be decoded as an ordinary phoneme and
    // reported as an unrequested word.
    BOOST_CHECK(refused("fixture-align-unpromised-breath").find("non-speech") !=
                std::string::npos);

    // The silence label is what a host filters the gaps out by, so a label the vocabulary does
    // not list among its silent phonemes would make that filter miss.
    BOOST_CHECK(refused("fixture-align-unknown-silence").find("silence label") !=
                std::string::npos);

    // It is also what the words are separated by and what the decode treats as silence, so it
    // must be a class of the vocabulary, and it must be declared.
    BOOST_CHECK(refused("fixture-align-unclassed-silence").find("not a class") !=
                std::string::npos);
    BOOST_CHECK(refused("fixture-align-no-silence").find("silence label") != std::string::npos);

    // A host judges whether the result fits a singer by the declared phonemes. A list that omits
    // a phoneme the model can emit, or includes a phoneme it cannot emit, is therefore rejected.
    const auto phonemes = refused("fixture-align-wrong-phonemes");
    BOOST_CHECK(phonemes.find("not listed: b") != std::string::npos);
    BOOST_CHECK(phonemes.find("listed but unknown: c") != std::string::npos);
}

/// The reference assumes that the separator is class zero. The released vocabulary satisfies this
/// assumption, but the provider reads the class from the vocabulary, so a model with a different
/// numbering decodes identically.
BOOST_AUTO_TEST_CASE(test_Hfa_FindsTheSeparatorThroughTheVocabulary) {
    Host host;
    const auto schedule = readSchedule();
    auto spec = host.load("fixture-align-reordered");
    auto made = AlignApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
    auto analyzer = made.take();

    AlignApi::AlignStartInput input;
    lyrics(input, "a b");
    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();
    BOOST_CHECK(texts(*result) == (std::vector<std::string>{"SP", "a", "AP", "b", "SP"}));
    BOOST_REQUIRE_EQUAL(result->words.size(), 5u);
    BOOST_CHECK_SMALL(result->words[1].start - schedule.span, schedule.frame);
    BOOST_CHECK_SMALL(result->words[3].start - 3 * schedule.span, schedule.frame);
}

/// No word fits in less than one frame, and an empty result would be indistinguishable from "none
/// of the lyrics was sung", so audio shorter than one frame is rejected as an invalid argument.
BOOST_AUTO_TEST_CASE(test_Hfa_RefusesAudioShorterThanAFrame) {
    Host host;
    auto analyzer = host.open();

    AlignApi::AlignStartInput input;
    lyrics(input, "a b");
    input.audio.samples.resize(100);
    auto refused = analyzer->start(input);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().code() == srt::Error::InvalidArgument);
    BOOST_CHECK(refused.error().message().find("one frame") != std::string::npos);
}

/// A stop requested before or after the model makes the execution report Cancelled and no
/// result, and it does not carry over to the next execution. The stop is requested from the
/// progress callback, which the provider calls on the executing thread, so the timing is not left
/// to a race with a model that takes milliseconds.
BOOST_AUTO_TEST_CASE(test_Hfa_ReportsACancellation) {
    Host host;
    auto analyzer = host.open();

    for (const double at : {0.0, 0.5, 0.85}) {
        AlignApi::AlignStartInput input;
        lyrics(input, "a b");
        input.progress = [&analyzer, at](double value) {
            if (value == at) {
                (void) analyzer->stop();
            }
        };
        auto stopped = analyzer->start(input);
        BOOST_REQUIRE(!stopped);
        BOOST_CHECK(stopped.error().code() == otter::AnalysisError::Cancelled);
        BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Canceled);
    }

    AlignApi::AlignStartInput again;
    lyrics(again, "a b");
    auto produced = analyzer->start(again);
    BOOST_CHECK_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}

BOOST_AUTO_TEST_SUITE_END()
