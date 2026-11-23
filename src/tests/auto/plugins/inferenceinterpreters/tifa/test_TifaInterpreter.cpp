// The tifa provider against a real five-graph export.
//
// The fixture's graphs are a schedule rather than a model: the spectrogram is the mean of each hop
// of the waveform broadcast over its mel bins, the score of a token peaks where that token sits in
// the phrase, and the text graphs are the exporter's own layout with its arithmetic left out. So
// what this proves is the provider's half of the work: the lyrics reach the dictionary and become
// the grid the text graphs read, the sound reaches the graphs as the span the host named, the
// decode places the words on the host's timeline in seconds, the gaps are named, and the
// declaration is checked against the files it promises before the package can load.
//
// Nothing here is asserted about how well the words fit the audio: these graphs do not know what
// singing looks like, so what is checked is the shape of the answer: ordered, contiguous, whole,
// and in the notation the declaration promised.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <stdcorelib/plugin/plugin.h>

#include <dsinfer/Api/Drivers/Onnx/OnnxDriverApi.h>
#include <dsinfer/Inference/InferenceDriverFactory.h>

#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/SVS/InferenceContrib.h>
#include <synthrt/SVS/InferenceInterpreterPlugin.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/AnalysisExecutive.h>
#include <otter/Api/Align/1/AlignApiL1.h>

#include <onnxutil/OnnxSupport.h>
#include <tifa/TifaInterpreter.h>

// Before TestSupport.h, which includes Boost.Test itself: including that header first would set its
// include guard and this main would never be generated.
#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

#include "TestSupport.h"

namespace fs = std::filesystem;
namespace AlignApi = otter::Api::Align::L1;
namespace CommonApi = otter::Api::Common::L1;
namespace OnnxApi = ds::Api::Onnx;

namespace {

    // The plugin that main.cpp of tifa declares, around the interpreter that this test
    // links. The test registers it as a static plugin with the plugin.json of tifa, so that
    // discovery selects this class rather than the library in the plugin tree.
    class TifaPlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(const srt::ContribContractTriplet &triplet) override {
            if (auto served = otter::onnx::checkServed(
                    triplet, srt::ContribContractTriplet(AlignApi::Contract.interfaceId(),
                                                         AlignApi::Contract.level(),
                                                         otter::tifa::Triplet.variant()));
                !served) {
                return served.takeError();
            }
            return std::unique_ptr<srt::ContribInterpreter>(new otter::tifa::TifaInterpreter());
        }
    };

}

STDC_EXPORT_STATIC_PLUGIN(TifaPlugin, srt::InferenceInterpreterPlugin::IID,
                          otter::test::pluginManifest())

namespace {

    constexpr int RATE = 48000;

    // Ten milliseconds of audio, the frame the export's front end advances by.
    constexpr double FRAME = 0.01;

    // The package the good fixtures live under, and the packages that must not load.
    //
    // Each of the bad ones carries one fact its declaration contradicts, so what the test asserts
    // is both that it is refused and which fact the refusal is about.
    constexpr char PACKAGE[] = "fixture-align-tifa";
    constexpr char WRONG_RATE[] = "fixture-align-tifa-wrong-rate";
    constexpr char NO_DICTIONARY[] = "fixture-align-tifa-no-dictionary";
    constexpr char MISSING_DICTIONARY[] = "fixture-align-tifa-missing-dictionary";
    constexpr char WRONG_PHONEMES[] = "fixture-align-tifa-wrong-phonemes";
    constexpr char UNKNOWN_SILENCE[] = "fixture-align-tifa-unknown-silence";
    constexpr char EXTRA_DICTIONARY[] = "fixture-align-tifa-extra-dictionary";

    // The packages that declare a capability the variant's own graphs do not have. Unlike the ones
    // above, no file of the model's own contradicts them.
    constexpr char TWO_CHANNELS[] = "fixture-align-tifa-two-channels";
    constexpr char UNKNOWN_SCHEME[] = "fixture-align-tifa-unknown-scheme";
    constexpr char PROMISED_NON_SPEECH[] = "fixture-align-tifa-promised-non-speech";

    struct DataOrSkip {
        DataOrSkip() {
            if (!fs::is_directory(fs::path(OTTER_TEST_FIXTURE_DIR) / PACKAGE)) {
                std::cerr << "SKIP: no tifa model fixture under " OTTER_TEST_FIXTURE_DIR "\n";
                std::exit(OTTER_TEST_SKIP_EXIT_CODE);
            }
            if (!fs::is_directory(fs::path(OTTER_TEST_DRIVER_PLUGIN_DIR)) ||
                std::string_view(OTTER_TEST_ONNXRUNTIME_DIR).empty()) {
                std::cerr << "SKIP: no ONNX driver plugin or ONNX Runtime in this tree\n";
                std::exit(OTTER_TEST_SKIP_EXIT_CODE);
            }
        }
    };

    struct Host {
        Host() {
            std::vector<fs::path> driverPaths = {fs::path(OTTER_TEST_DRIVER_PLUGIN_DIR)};
            factory.setPluginPaths(driverPaths);
            auto loader = factory.find(OnnxApi::Contract.name());
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
        }

        ~Host() {
            package.reset();
        }

        srt::ContribSpec *load(const std::string &name = PACKAGE) {
            return loadFrom(fs::path(OTTER_TEST_FIXTURE_DIR) / name);
        }

        srt::ContribSpec *loadFrom(const fs::path &directory) {
            auto opened = unit.openPackage(directory, srt::SynthUnit::Load);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), otter::test::why(opened));
            package = opened.take();
            auto spec = package.contribution(srt::InferenceCategory::NAME, "align");
            BOOST_REQUIRE(spec != nullptr);
            return spec;
        }

        std::unique_ptr<AlignApi::Executive> open() {
            return openFrom(fs::path(OTTER_TEST_FIXTURE_DIR) / PACKAGE);
        }

        std::unique_ptr<AlignApi::Executive> openFrom(const fs::path &directory) {
            auto spec = loadFrom(directory);
            auto made = AlignApi::Executive::create(*spec->as<srt::InferenceSpec>());
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
            return made.take();
        }

        ds::InferenceDriverFactory factory;
        srt::SynthUnit unit;
        srt::PackageHandle package;
    };

    // Returns the root cause of the load failure of the fixture package \a name, which the caller
    // expects to fail to load. The two cases that read a refusal by name share this reader.
    srt::Error loadRefusal(Host &host, const char *name) {
        auto opened =
            host.unit.openPackage(fs::path(OTTER_TEST_FIXTURE_DIR) / name, srt::SynthUnit::Load);
        BOOST_REQUIRE(!opened);
        return opened.error().rootCause();
    }

    // One written unit of a dictionary, and the phonemes that dictionary spells it with.
    struct Entry {
        std::string written;
        std::vector<std::string> phonemes;
    };

    // Reads a dictionary file: one entry per line, the written unit, a tab, then its phonemes.
    std::vector<Entry> readDictionary(const fs::path &path) {
        std::ifstream file(path);
        BOOST_REQUIRE_MESSAGE(file.is_open(), "the fixture dictionary should be readable");
        std::vector<Entry> entries;
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const auto tab = line.find('\t');
            if (tab == std::string::npos) {
                continue;
            }
            Entry entry;
            entry.written = line.substr(0, tab);
            std::istringstream phonemes(line.substr(tab + 1));
            std::string phoneme;
            while (phonemes >> phoneme) {
                entry.phonemes.push_back(phoneme);
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    // What the fixture's own dictionary says about the lyrics to use.
    //
    // The words are read out of the dictionary rather than written here, because which written
    // units the fixture carries is the fixture's business: what the test is about is that the
    // words a caller asks for are looked up, placed, and reported back in the dictionary's own
    // phonemes. The dictionary of the language the package aligns by default is the one whose
    // phonemes the declaration lists for that language: the other language's are its own set.
    struct Lyrics {
        std::vector<std::string> written;
        std::set<std::string> phonemes;

        std::string text() const {
            std::string joined;
            for (const auto &word : written) {
                joined += (joined.empty() ? "" : " ") + word;
            }
            return joined;
        }
    };

    Lyrics fixtureLyrics(const AlignApi::Exports &schema) {
        const auto directory = fs::path(OTTER_TEST_FIXTURE_DIR) / PACKAGE / "dictionaries";
        BOOST_REQUIRE_MESSAGE(fs::is_directory(directory),
                              "the fixture should carry its dictionaries beside the declaration");
        // The declared phonemes of the default language, which is what tells the dictionaries
        // apart without the test having to know how the fixture named their files.
        std::set<std::string> declared;
        for (const auto &entry : schema.languages) {
            if (entry.language == schema.defaultLanguage) {
                declared.insert(entry.phonemes.begin(), entry.phonemes.end());
            }
        }
        BOOST_REQUIRE_MESSAGE(!declared.empty(),
                              "the declaration should list the phonemes of its default language");

        std::vector<Entry> chosen;
        for (const auto &candidate : fs::directory_iterator(directory)) {
            if (!candidate.is_regular_file()) {
                continue;
            }
            const auto entries = readDictionary(candidate.path());
            if (entries.empty()) {
                continue;
            }
            const bool matches =
                std::all_of(entries.begin(), entries.end(), [&declared](const Entry &entry) {
                    return std::all_of(entry.phonemes.begin(), entry.phonemes.end(),
                                       [&declared](const std::string &phoneme) {
                                           return declared.count(phoneme) != 0;
                                       });
                });
            if (matches) {
                chosen = entries;
                break;
            }
        }
        BOOST_REQUIRE_MESSAGE(!chosen.empty(),
                              "the fixture should carry the dictionary of its default language");

        Lyrics lyrics;
        for (const auto &entry : chosen) {
            lyrics.written.push_back(entry.written);
            lyrics.phonemes.insert(entry.phonemes.begin(), entry.phonemes.end());
            if (lyrics.written.size() == 2) {
                break;
            }
        }
        return lyrics;
    }

    // Audio of \a seconds at the rate the package declares, starting \a startTime into the
    // caller's timeline. Silence is enough: the fixture's front end averages it, and the graphs
    // read the schedule rather than the sound. The count is rounded rather than truncated, since
    // the product of a rate and a length lands a hair under the whole number of samples more often
    // than it lands on it, and a sample short is a span a hair shorter than the caller asked for.
    CommonApi::AudioSegment tone(double seconds = 1.0, double startTime = 0) {
        CommonApi::AudioSegment audio;
        audio.sampleRate = RATE;
        audio.channelCount = 1;
        audio.samples.assign(static_cast<std::size_t>(std::llround(seconds * RATE)), 0.0f);
        audio.startTime = startTime;
        return audio;
    }

    // One span of audio and the lyrics to align it against. The input is the caller's, because a
    // start input is not copyable: the contract hands the provider the caller's own object.
    void ask(AlignApi::StartInput &input, const Lyrics &lyrics, double seconds = 1.0,
             double startTime = 0) {
        input.audio = tone(seconds, startTime);
        input.lyrics = lyrics.text();
    }

    std::vector<std::string> texts(const AlignApi::Result &result) {
        std::vector<std::string> out;
        for (const auto &word : result.words) {
            out.push_back(word.text);
        }
        return out;
    }

    // A phoneme label with any language prefix taken off it. A result reports the language it
    // aligned in without that language's prefix and keeps a phoneme borrowed from another one,
    // whose spelling says where it came from.
    std::string bare(const std::string &label) {
        const auto slash = label.find('/');
        return slash == std::string::npos ? label : label.substr(slash + 1);
    }


    // Asserts the invariants every result owes its caller whatever the model said: words in order,
    // each ending where the next begins, the first starting where the span does and the last
    // ending where it ends, and no word or phone of no length.
    void checkCoverage(const AlignApi::Result &result, double startTime, double seconds) {
        BOOST_REQUIRE(!result.words.empty());
        BOOST_CHECK_CLOSE(result.words.front().start, startTime, 1e-6);
        for (std::size_t i = 0; i < result.words.size(); ++i) {
            const auto &word = result.words[i];
            BOOST_CHECK_GT(word.duration, 0.0);
            if (i > 0) {
                const auto &previous = result.words[i - 1];
                BOOST_CHECK_SMALL(word.start - (previous.start + previous.duration), 1e-9);
            }
            for (std::size_t p = 0; p < word.phones.size(); ++p) {
                const auto &phone = word.phones[p];
                BOOST_CHECK_GT(phone.duration, 0.0);
                // A phone of no label is a slot the result has nothing to report for, and a caller
                // cannot read one as a phoneme: whatever the graphs answered, a result reports the
                // phonemes of the language it aligned in or it fails.
                BOOST_CHECK_MESSAGE(!phone.text.empty(),
                                    "a phone of a result carries a phoneme, not an empty label");
                if (p > 0) {
                    const auto &previous = word.phones[p - 1];
                    BOOST_CHECK_SMALL(phone.start - (previous.start + previous.duration), 1e-9);
                }
            }
            if (!word.phones.empty()) {
                // A word's phones cover it exactly, which is what lets a host read a phone's span
                // without looking at the word around it.
                BOOST_CHECK_CLOSE(word.phones.front().start, word.start, 1e-9);
                const auto &last = word.phones.back();
                BOOST_CHECK_SMALL(last.start + last.duration - (word.start + word.duration), 1e-9);
            }
        }
        const auto &last = result.words.back();
        BOOST_CHECK_SMALL(last.start + last.duration - (startTime + seconds), 1e-6);
    }

}

BOOST_TEST_GLOBAL_FIXTURE(DataOrSkip);

BOOST_AUTO_TEST_SUITE(test_TifaInterpreter)

BOOST_AUTO_TEST_CASE(test_TifaInterpreter_ReportsWhatTheDeclarationExports) {
    Host host;
    auto spec = host.load();
    auto schema = spec->exports()->as<AlignApi::Exports>();
    BOOST_REQUIRE(schema != nullptr);

    // The exports are the declaration's own words, read through the contract's reader, and the
    // provider has checked them against the model's config.json and vocabulary.json before the
    // package could load.
    BOOST_CHECK_EQUAL(schema->sampleRate, RATE);
    BOOST_CHECK_EQUAL(schema->channelCount, 1);
    BOOST_CHECK_CLOSE(schema->maxSegmentDuration, 60.0, 1e-9);
    BOOST_CHECK_EQUAL(schema->defaultLanguage, "cmn");
    // The label a gap carries is this variant's own spelling. The model's vocabulary has no
    // silence symbol at all, which is why the label is checked against the phoneme sets rather
    // than looked up in the vocabulary.
    BOOST_CHECK_EQUAL(schema->silenceLabel, "SP");
    // The model has no non-speech head: it cannot report a breath, so it promises none.
    BOOST_CHECK(schema->nonSpeechPhonemes.empty());
    BOOST_CHECK(schema->defaultNonSpeechPhonemes.empty());

    // A language is declared with the scheme of its phonemes, the form of its lyrics, and the
    // phonemes a result in it can contain.
    BOOST_REQUIRE_EQUAL(schema->languages.size(), 2u);
    const auto declared = [&schema](const std::string &language) {
        for (const auto &entry : schema->languages) {
            if (entry.language == language) {
                return entry;
            }
        }
        BOOST_FAIL("the exports should list " + language);
        return schema->languages.front();
    };
    const auto mandarin = declared("cmn");
    BOOST_CHECK_EQUAL(mandarin.scheme, "pinyin");
    BOOST_CHECK(mandarin.lyrics == AlignApi::LyricsForm::Scheme);
    BOOST_CHECK(!mandarin.phonemes.empty());
    const auto english = declared("eng");
    BOOST_CHECK_EQUAL(english.scheme, "arpabet");
    BOOST_CHECK(!english.phonemes.empty());
}

BOOST_AUTO_TEST_CASE(test_TifaInterpreter_PlacesTheWordsTheLyricsName) {
    Host host;
    auto schema = host.load()->exports()->as<AlignApi::Exports>();
    BOOST_REQUIRE(schema != nullptr);
    const auto words = fixtureLyrics(*schema);

    auto analyzer = host.open();

    // A span that does not begin at zero, because that is the case the time base matters in: read
    // as offsets, every boundary below would be thirty seconds early.
    constexpr double SPAN_START = 30.0;
    constexpr double SECONDS = 1.0;

    AlignApi::StartInput input;
    ask(input, words, SECONDS, SPAN_START);
    std::vector<double> reported;
    input.progress = [&reported](double value) {
        reported.push_back(value);
        return true;
    };

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    // The result records the language and scheme of its phonemes, because the caller may have left
    // both to the declared defaults.
    BOOST_CHECK_EQUAL(result->language, "cmn");
    BOOST_CHECK_EQUAL(result->scheme, "pinyin");

    // The words the caller asked for come back in the order they were asked for, and a label of
    // the module's own appears only where the model placed no word at all.
    std::vector<std::string> placed;
    for (const auto &text : texts(*result)) {
        if (text != schema->silenceLabel) {
            placed.push_back(text);
        }
    }
    BOOST_CHECK(placed == words.written);

    checkCoverage(*result, SPAN_START, SECONDS);

    // Every phoneme a result reports is one of the dictionary's own, with the language prefix the
    // model spells it with taken off for the default language.
    for (const auto &word : result->words) {
        for (const auto &phone : word.phones) {
            BOOST_CHECK_MESSAGE(words.phonemes.count(bare(phone.text)) != 0,
                                "unexpected phoneme " + phone.text);
        }
    }
    // A word the dictionary resolved is placed with the phonemes it was written as: the fixture's
    // dictionary spells each of these words with at least one, and none of them may be dropped
    // without the word losing its place.
    for (const auto &word : result->words) {
        if (word.text == schema->silenceLabel) {
            // An inserted segment carries the label as its one phoneme: the contract expects the
            // label of the silence segment in PhoneInfo::text, and the other variant that inserts
            // segments reports that shape rather than no phonemes at all.
            BOOST_CHECK_MESSAGE(word.phones.size() == 1, "the silence segment reports one phoneme");
            if (word.phones.size() == 1) {
                BOOST_CHECK_EQUAL(word.phones.front().text, schema->silenceLabel);
                BOOST_CHECK_EQUAL(word.phones.front().duration, word.duration);
            }
            continue;
        }
        BOOST_CHECK(!word.phones.empty());
    }

    BOOST_REQUIRE(!reported.empty());
    BOOST_CHECK_CLOSE(reported.back(), 1.0, 1e-9);
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}

// The frames of a graph stop where the graph stopped, and the span does not: what neither a word
// nor a phoneme covers is named with the declared silence label, which is what makes a result
// cover the span rather than end somewhere inside it.
BOOST_AUTO_TEST_CASE(test_TifaInterpreter_NamesWhatTheModelPlacedNoWordOver) {
    Host host;
    auto schema = host.load()->exports()->as<AlignApi::Exports>();
    BOOST_REQUIRE(schema != nullptr);
    const auto words = fixtureLyrics(*schema);
    auto analyzer = host.open();

    // A span of a hundred and a half frames: the graphs see a hundred full frames of it, and five
    // milliseconds are left over that no frame of theirs covers.
    constexpr double SECONDS = 1.005;

    AlignApi::StartInput input;
    ask(input, words, SECONDS);
    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    checkCoverage(*result, 0.0, SECONDS);

    const auto &last = result->words.back();
    BOOST_CHECK_EQUAL(last.text, schema->silenceLabel);
    // The inserted segment reports its label as the one phoneme that covers it, the shape the
    // contract describes for a phone that belongs to a silence segment.
    BOOST_CHECK_MESSAGE(last.phones.size() == 1, "the trailing silence reports one phoneme");
    if (last.phones.size() == 1) {
        BOOST_CHECK_EQUAL(last.phones.front().text, schema->silenceLabel);
        BOOST_CHECK_EQUAL(last.phones.front().duration, last.duration);
    }
    // The gap is the part of the span the frames do not reach, and nothing more.
    BOOST_CHECK_GT(last.duration, 0.0);
    BOOST_CHECK_LE(last.duration, FRAME + 1e-6);
    BOOST_CHECK_SMALL(last.start - FRAME * 100, 1e-6);
    BOOST_CHECK_GT(result->words.size(), 1u);
}

BOOST_AUTO_TEST_CASE(test_TifaInterpreter_RefusesWhatItCannotHonor) {
    Host host;
    auto schema = host.load()->exports()->as<AlignApi::Exports>();
    BOOST_REQUIRE(schema != nullptr);
    const auto words = fixtureLyrics(*schema);
    auto analyzer = host.open();

    // A span at a rate the package does not declare: the host prepared the wrong audio, and
    // resampling it silently would hide that.
    AlignApi::StartInput wrongRate;
    ask(wrongRate, words);
    wrongRate.audio.sampleRate = 44100;
    auto refusedRate = analyzer->start(wrongRate);
    BOOST_REQUIRE(!refusedRate);
    BOOST_CHECK(refusedRate.error().message().find("48000") != std::string::npos);

    // An aligner that is not told what to look for has nothing to align, and answering with a
    // transcription instead would be a different contract.
    AlignApi::StartInput noLyrics;
    ask(noLyrics, words);
    noLyrics.lyrics = "";
    auto empty = analyzer->start(noLyrics);
    BOOST_REQUIRE(!empty);
    BOOST_CHECK(empty.error().message().find("lyrics") != std::string::npos);

    AlignApi::StartInput unknownLanguage;
    ask(unknownLanguage, words);
    unknownLanguage.language = "qqq";
    BOOST_CHECK(!analyzer->start(unknownLanguage));

    // A scheme not declared for the language is rejected rather than ignored, because the returned
    // phonemes would otherwise be in a notation other than the requested one.
    AlignApi::StartInput unknownScheme;
    ask(unknownScheme, words);
    unknownScheme.scheme = "arpabet";
    auto wrongScheme = analyzer->start(unknownScheme);
    BOOST_REQUIRE(!wrongScheme);
    BOOST_CHECK(wrongScheme.error().code() == srt::Error::InvalidArgument);
    // One sentence: what the caller asked for, then the capability this model lacks.
    BOOST_CHECK(wrongScheme.error().message().find("the caller asked for the language cmn with "
                                                   "the scheme arpabet, which this model cannot "
                                                   "align") != std::string::npos);

    // A label the module did not declare is refused rather than ignored: having looked for a breath
    // and found none is a different answer from not being able to look.
    AlignApi::StartInput unknownLabel;
    ask(unknownLabel, words);
    unknownLabel.nonSpeechPhonemes = {"AP"};
    auto unheadlined = analyzer->start(unknownLabel);
    BOOST_REQUIRE(!unheadlined);
    BOOST_CHECK(unheadlined.error().code() == srt::Error::InvalidArgument);
    BOOST_CHECK(unheadlined.error().message().find("non-speech phoneme AP") != std::string::npos);
    // The wording is the one the hfa provider and the stub use for the same condition, so a
    // message that drifted from it fails here rather than in one provider only.
    BOOST_CHECK(unheadlined.error().message().find(", which this model cannot report") !=
                std::string::npos);

    // A word the dictionary has never heard of: it would be a hole in the alignment, and where the
    // hole ends cannot be recovered from the result afterwards.
    AlignApi::StartInput unknownLyrics;
    ask(unknownLyrics, words);
    unknownLyrics.lyrics = "zzqqx";
    auto unknown = analyzer->start(unknownLyrics);
    BOOST_REQUIRE(!unknown);
    BOOST_CHECK(unknown.error().message().find("dictionary") != std::string::npos);
}

// A host that stops what it started gets a cancellation and no result, on an analyzer that runs
// the next execution afterwards: a stop belongs to the execution it was aimed at.
// The release package, when the host fetched one, runs the trained weights. The fixture cases pin
// the arithmetic of the provider. This one proves that the weights load and that the result keeps
// the coverage the contract promises for any model.
BOOST_AUTO_TEST_CASE(test_TifaInterpreter_AlignsWithTheTrainedModel) {
    Host host;
    auto spec = host.loadFrom(otter::test::realModelsRoot() / "otter-tifa");
    auto made = AlignApi::Executive::create(*spec->as<srt::InferenceSpec>());
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), otter::test::why(made));
    auto analyzer = made.take();

    AlignApi::StartInput input;
    input.audio = tone(1.5, 0.5);
    input.lyrics = "ni hao";

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    checkCoverage(*result, 0.5, 1.5);
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Succeeded);
}
BOOST_AUTO_TEST_CASE(test_TifaInterpreter_ReportsAStopAsACancellation) {
    Host host;
    auto schema = host.load()->exports()->as<AlignApi::Exports>();
    BOOST_REQUIRE(schema != nullptr);
    const auto words = fixtureLyrics(*schema);
    auto analyzer = host.open();

    AlignApi::StartInput input;
    ask(input, words);
    // The first progress report is the start of the execution, which is early enough that nothing
    // has been run yet: stopping there is stopping the execution before it did any work.
    input.progress = [&analyzer](double value) {
        if (value <= 0.0) {
            (void) analyzer->stop();
        }
        return true;
    };
    auto stopped = analyzer->start(input);
    BOOST_REQUIRE(!stopped);
    BOOST_CHECK(stopped.error().code() == otter::AnalysisError::Cancelled);
    BOOST_CHECK_EQUAL(analyzer->state(), srt::ITask::Canceled);
    BOOST_CHECK(analyzer->waitForFinished());

    AlignApi::StartInput again;
    ask(again, words);
    auto after = analyzer->start(again);
    BOOST_CHECK_MESSAGE(static_cast<bool>(after), otter::test::why(after));

    // Stopping an analyzer that is not running is not an error: it is the path a host takes when
    // it tears something down that it had already waited for.
    BOOST_CHECK(analyzer->stop());
    BOOST_CHECK(analyzer->waitForFinished());
    BOOST_CHECK_NE(analyzer->state(), srt::ITask::Running);
}

// A declaration must agree with the files it names. Each of these packages carries one fact the
// declaration contradicts, and the provider reads the model's own files at load, so the refusal
// lands where both are in hand rather than at the end of a run that produced an answer nobody can
// tell is wrong. The packages and why each one is a defect: scripts/make-model-fixtures.py.
BOOST_AUTO_TEST_CASE(test_TifaInterpreter_RefusesDeclarationsItsModelCannotHonor) {
    Host host;

    // One package, what its refusal has to name, and why the declaration cannot be honored. A
    // symbol every language shares may be listed by any of them, which is what keeps the shared
    // ones from reading as phonemes the model cannot emit.
    struct Refusal {
        const char *package;
        std::vector<const char *> named;
        const char *why;
    };
    const std::vector<Refusal> refusals = {
        {WRONG_RATE,         {"16000"},          "a host preparing audio at another rate aligns at the wrong speed"},
        {NO_DICTIONARY,      {"dictionary"},     "a declared language is given no dictionary at all"               },
        {MISSING_DICTIONARY, {"dictionary"},     "the dictionary key names a file that is not there"               },
        {WRONG_PHONEMES,
         {"not listed:", "listed but unknown:"},
         "the list omits one the model can emit and carries one it cannot"                                         },
        {UNKNOWN_SILENCE,    {"silence label"},  "the silence label is also a declared phoneme"                    },
    };
    for (const auto &refusal : refusals) {
        const auto message = loadRefusal(host, refusal.package).message();
        for (const auto *fragment : refusal.named) {
            BOOST_CHECK_MESSAGE(message.find(fragment) != std::string::npos,
                                refusal.package << ": " << refusal.why << "; it should name "
                                                << fragment << ", and it said: " << message);
        }
    }

    // A dictionary for a language the declaration does not list is a file nobody will read, and
    // the declaration is still usable: refusing it would make a package that dropped a language
    // mid-packaging unloadable rather than smaller.
    auto extra = host.unit.openPackage(fs::path(OTTER_TEST_FIXTURE_DIR) / EXTRA_DICTIONARY,
                                       srt::SynthUnit::Load);
    BOOST_CHECK_MESSAGE(static_cast<bool>(extra), otter::test::why(extra));
}

// A declaration is checked against what the variant's own graphs can do, not only against the
// files they are shipped with. Each of these three promises something the model has no way to
// honor, and honoring it silently would put a wrong answer beyond the host's reach: audio that the
// provider mixed down without being asked, phonemes read in a notation nobody declared, or a
// search that found nothing where nothing was looked for.
BOOST_AUTO_TEST_CASE(test_TifaInterpreter_RefusesPromisesItsModelCannotKeep) {
    Host host;

    // The five graphs take one channel each, so a declaration of two describes audio that the host
    // prepares and the provider would reduce before its model saw it.
    const auto twoChannels = loadRefusal(host, TWO_CHANNELS);
    BOOST_CHECK(twoChannels.code() == srt::Error::InvalidFormat);
    BOOST_CHECK(twoChannels.message().find("the tifa variant feeds its model one channel") !=
                std::string::npos);
    BOOST_CHECK(twoChannels.message().find("declare 2") != std::string::npos);

    // A scheme the variant does not read is refused rather than guessed at: the same language in
    // another notation carries other phonemes, and a dictionary read as if it were in that notation
    // would align a different phoneme set.
    const auto unknownScheme = loadRefusal(host, UNKNOWN_SCHEME);
    BOOST_CHECK(unknownScheme.code() == srt::Error::InvalidFormat);
    BOOST_CHECK(unknownScheme.message().find("reads the schemes pinyin, jyutping, romaji and "
                                             "arpabet") != std::string::npos);
    BOOST_CHECK(unknownScheme.message().find("the exports declare x-sampa for cmn") !=
                std::string::npos);

    // The model has no non-speech head: it can report neither a breath nor a cough, so promising
    // one would make a search that found nothing and a search that never ran the same answer.
    const auto promisedNonSpeech = loadRefusal(host, PROMISED_NON_SPEECH);
    BOOST_CHECK(promisedNonSpeech.code() == srt::Error::InvalidFormat);
    BOOST_CHECK(promisedNonSpeech.message().find("detects no non-speech sound") !=
                std::string::npos);
    BOOST_CHECK(promisedNonSpeech.message().find("cannot promise the phoneme AP") !=
                std::string::npos);
}

// A word the dictionary spells with a phoneme the vocabulary has no symbol for is left out of the
// phrase rather than handed over: a missing symbol looks up to zero, which is the padding between
// tokens, so aligning one would answer about a word the model was never asked about. The fixture's
// own dictionaries are consistent with its vocabulary, so a copy of the package is written with
// one such entry added, and loaded as a package of its own.
BOOST_AUTO_TEST_CASE(test_TifaInterpreter_DropsAWordTheVocabularyCannotSpell) {
    // A directory that goes away with the test, whatever it did.
    struct Scratch {
        fs::path path;

        ~Scratch() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    };

    const auto source = fs::path(OTTER_TEST_FIXTURE_DIR) / PACKAGE;
    Scratch scratch{fs::temp_directory_path() / (std::string(PACKAGE) + "-unspellable")};
    fs::remove_all(scratch.path);
    fs::copy(source, scratch.path, fs::copy_options::recursive);

    // The dictionary of the language the package aligns by default, told from the others by the
    // phonemes the declaration lists for it, exactly as the lyrics are chosen above.
    Host probe;
    auto probeSchema = probe.load()->exports()->as<AlignApi::Exports>();
    BOOST_REQUIRE(probeSchema != nullptr);
    const auto words = fixtureLyrics(*probeSchema);
    std::set<std::string> declared;
    for (const auto &entry : probeSchema->languages) {
        if (entry.language == probeSchema->defaultLanguage) {
            declared.insert(entry.phonemes.begin(), entry.phonemes.end());
        }
    }
    fs::path dictionary;
    for (const auto &candidate : fs::directory_iterator(scratch.path / "dictionaries")) {
        const auto entries = readDictionary(candidate.path());
        if (entries.empty()) {
            continue;
        }
        const bool matches =
            std::all_of(entries.begin(), entries.end(), [&declared](const Entry &entry) {
                return std::all_of(entry.phonemes.begin(), entry.phonemes.end(),
                                   [&declared](const std::string &phoneme) {
                                       return declared.count(phoneme) != 0;
                                   });
            });
        if (matches) {
            dictionary = candidate.path();
            break;
        }
    }
    BOOST_REQUIRE_MESSAGE(!dictionary.empty(), "the fixture should carry its default dictionary");

    // A written unit no language spells, added where the dictionary keeps its own entries: the word
    // is unknown to the vocabulary, so it is unknown to the phrase.
    constexpr char UNSPELLABLE[] = "zq";
    {
        std::ofstream file(dictionary, std::ios::app);
        BOOST_REQUIRE(file.is_open());
        file << UNSPELLABLE << '\t' << "qq\n";
    }

    Host host;
    auto analyzer = host.openFrom(scratch.path);

    // Alone, the phrase resolves to nothing at all, which is refused rather than answered with
    // silence: an empty result would read as none of these words being in this audio.
    AlignApi::StartInput alone;
    ask(alone, words);
    alone.lyrics = UNSPELLABLE;
    auto refused = analyzer->start(alone);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().message().find("dictionary") != std::string::npos);

    // Beside a word the dictionary does know, the phrase keeps the word it can spell and drops the
    // one it cannot, and the drop is not reported as a stretch of silence.
    AlignApi::StartInput mixed;
    ask(mixed, words);
    mixed.lyrics = words.text() + " " + UNSPELLABLE;
    auto produced = analyzer->start(mixed);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    std::vector<std::string> placed;
    for (const auto &text : texts(*result)) {
        if (text != probeSchema->silenceLabel) {
            placed.push_back(text);
        }
    }
    BOOST_CHECK(placed == words.written);
    checkCoverage(*result, 0.0, 1.0);
}

// A written unit a dictionary spells more than one way is one word with a reading per line of its
// entry, and which of them a result reports is the answer of the host's whole-word program rather
// than the first line of the file. The fixture adds a second reading to one entry in a language
// that is not the default one, and its score graph is the exporter's layout with the arithmetic
// left out, so what its table settles is a tie (scripts/make-model-fixtures.py).
BOOST_AUTO_TEST_CASE(test_TifaInterpreter_PicksOneReadingOfAWordTheDictionarySpellsTwice) {
    // A directory that goes away with the test, whatever it did.
    struct Scratch {
        fs::path path;

        ~Scratch() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    };

    // The language whose dictionary is the only one the fixture gives several readings for.
    constexpr char LANGUAGE[] = "eng";

    const auto source = fs::path(OTTER_TEST_FIXTURE_DIR) / PACKAGE;
    Scratch scratch{fs::temp_directory_path() / (std::string(PACKAGE) + "-readings")};
    fs::remove_all(scratch.path);
    fs::copy(source, scratch.path, fs::copy_options::recursive);

    // That language's dictionary is told from the other one by the phonemes the declaration lists
    // for it, exactly as the default language's dictionary is found above.
    Host probe;
    auto schema = probe.load()->exports()->as<AlignApi::Exports>();
    BOOST_REQUIRE(schema != nullptr);
    std::set<std::string> declared;
    for (const auto &language : schema->languages) {
        if (language.language == LANGUAGE) {
            declared.insert(language.phonemes.begin(), language.phonemes.end());
        }
    }
    BOOST_REQUIRE_MESSAGE(!declared.empty(),
                          "the fixture declaration should name that language's phonemes");

    std::vector<Entry> entries;
    fs::path dictionary;
    for (const auto &candidate : fs::directory_iterator(scratch.path / "dictionaries")) {
        const auto read = readDictionary(candidate.path());
        if (read.empty()) {
            continue;
        }
        const bool matches = std::all_of(read.begin(), read.end(), [&declared](const Entry &entry) {
            return std::all_of(
                entry.phonemes.begin(), entry.phonemes.end(),
                [&declared](const std::string &phoneme) { return declared.count(phoneme) != 0; });
        });
        if (matches) {
            entries = read;
            dictionary = candidate.path();
            break;
        }
    }
    BOOST_REQUIRE_MESSAGE(!dictionary.empty(),
                          "the fixture should carry that language's dictionary");
    BOOST_REQUIRE_MESSAGE(entries.size() >= 3,
                          "that dictionary should carry several written units to choose from");

    // The second reading of one entry is spelled with the phonemes of two other entries, so it is
    // longer than the first: the two readings then disagree about a slot only the longer one
    // reaches, which is the slot the grid has to hold and the chosen reading has to leave empty.
    const auto twice = entries[0];
    const auto once = entries[1];
    std::vector<std::string> second = once.phonemes;
    second.insert(second.end(), entries[2].phonemes.begin(), entries[2].phonemes.end());
    {
        std::ofstream file(dictionary, std::ios::app);
        BOOST_REQUIRE(file.is_open());
        file << twice.written << '\t';
        for (const auto &phoneme : second) {
            file << phoneme << ' ';
        }
        file << '\n';
    }

    Host host;
    auto analyzer = host.openFrom(scratch.path);

    const auto phones = [](const AlignApi::WordInfo &word) {
        std::vector<std::string> out;
        for (const auto &phone : word.phones) {
            out.push_back(phone.text);
        }
        return out;
    };
    const auto written = [schema](const AlignApi::Result &result) {
        std::vector<const AlignApi::WordInfo *> out;
        for (const auto &word : result.words) {
            if (word.text != schema->silenceLabel) {
                out.push_back(&word);
            }
        }
        return out;
    };

    AlignApi::StartInput input;
    input.audio = tone();
    input.language = LANGUAGE;
    input.lyrics = twice.written + " " + once.written;
    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), otter::test::why(produced));
    auto result = produced.take();

    const auto placed = written(*result);
    BOOST_REQUIRE_MESSAGE(placed.size() == 2, "both words of the phrase should be placed");
    BOOST_CHECK_EQUAL(placed[0]->text, twice.written);
    BOOST_CHECK_EQUAL(placed[1]->text, once.written);
    // The reading the program answered with, as the dictionary spells it: the first line of the
    // entry for the word with two readings, and the only line of the other word's entry.
    BOOST_CHECK(phones(*placed[0]) == twice.phonemes);
    BOOST_CHECK(phones(*placed[1]) == once.phonemes);
    // A result reports the language it aligned in without that language's prefix. The word with two
    // readings is where this is not the same statement as the one above: the prefix is exactly what
    // the vocabulary spells those phonemes with, and a result that kept it would name phonemes the
    // declaration of the language never promised.
    for (const auto *word : placed) {
        for (const auto &phone : word->phones) {
            BOOST_CHECK_MESSAGE(phone.text.find('/') == std::string::npos,
                                "a result reports the language in use without its prefix");
            // The declaration is the promise a caller reads the labels against, so a label outside
            // it is one the caller has no meaning for, and an empty label is outside every
            // declaration. This is the invariant the placement of a compacted tensor broke: a row
            // of that tensor is not the row of the grid of the same number, and a row read against
            // the wrong grid row reports a reading's gap, which spells nothing at all.
            BOOST_CHECK_MESSAGE(declared.count(phone.text) != 0,
                                "a result reports only the phonemes its language declares");
        }
    }
    checkCoverage(*result, 0.0, 1.0);

    // The same phrase again: the reading is the program's own answer, and a run of it that depended
    // on the order a table arrived in would not answer twice alike.
    AlignApi::StartInput repeated;
    repeated.audio = tone();
    repeated.language = LANGUAGE;
    repeated.lyrics = input.lyrics;
    auto again = analyzer->start(repeated);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(again), otter::test::why(again));
    auto second_result = again.take();
    BOOST_REQUIRE_EQUAL(second_result->words.size(), result->words.size());
    for (std::size_t index = 0; index < result->words.size(); ++index) {
        const auto &expected = result->words[index];
        const auto &actual = second_result->words[index];
        BOOST_CHECK_EQUAL(actual.text, expected.text);
        BOOST_CHECK_EQUAL(actual.start, expected.start);
        BOOST_CHECK_EQUAL(actual.duration, expected.duration);
        BOOST_CHECK(phones(actual) == phones(expected));
    }
}

BOOST_AUTO_TEST_SUITE_END()
