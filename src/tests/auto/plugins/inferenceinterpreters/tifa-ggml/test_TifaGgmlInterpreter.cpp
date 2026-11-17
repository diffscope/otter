#include <system_error>
#define BOOST_TEST_MAIN
#include "InterpreterTestSupport.h"

BOOST_AUTO_TEST_CASE(test_TifaGgmlInterpreter_ReportsWhatTheDeclarationExports) {
    Host host;
    host.write();
    auto spec = host.load();
    auto schema = spec->exports()->as<AlignApi::Schema>();
    BOOST_REQUIRE(schema != nullptr);

    BOOST_CHECK_EQUAL(schema->sampleRate, RATE);
    BOOST_CHECK_EQUAL(schema->channelCount, 1);
    BOOST_CHECK_CLOSE(schema->maxSegmentDuration, 300.0, 1e-9);
    BOOST_CHECK_EQUAL(schema->silenceLabel, "SP");
    BOOST_REQUIRE_EQUAL(schema->languages.size(), 1u);
    BOOST_CHECK_EQUAL(schema->languages.front().language, "cmn");
    BOOST_CHECK_EQUAL(schema->languages.front().scheme, "pinyin");
    BOOST_CHECK(schema->languages.front().lyrics == AlignApi::LyricsForm::Scheme);
    // The variant reads no knobs: the engine takes no alignment knobs the contract could carry.
    BOOST_CHECK(!schema->nonSpeechThreshold.honored);
    BOOST_CHECK(!schema->nonSpeechMinDuration.honored);
    BOOST_CHECK(!schema->gapFill.honored);
}

BOOST_AUTO_TEST_CASE(test_TifaGgmlInterpreter_RefusesTheDeclarationsItCannotHonor) {
    // The engine detects no non-speech sound, so promising one would make "none found" and
    // "nothing was looked for" the same answer.
    {
        Host host;
        host.write(OTTER_TEST_FAKE_CLI, "models/tifa.gguf", "path/to/dicts",
                   R"({
                       "sampleRate": 48000,
                       "channelCount": 1,
                       "maxSegmentDuration": 300,
                       "languages": [
                           {"language": "cmn", "scheme": "pinyin", "lyrics": "scheme",
                            "phonemes": ["a", "b"]}
                       ],
                       "defaultLanguage": "cmn",
                       "silenceLabel": "SP",
                       "nonSpeechPhonemes": ["AP"],
                       "defaultNonSpeechPhonemes": ["AP"]
                   })",
                   R"({"cli": "x", "model": "y", "languages": {"cmn": "zh"}})");
        BOOST_CHECK(host.refusal().find("detects no non-speech sound") != std::string::npos);
    }
    // A language the exports list without an engine code leaves the G2P without a language.
    {
        Host host;
        host.write(OTTER_TEST_FAKE_CLI, "models/tifa.gguf", "path/to/dicts", EXPORTS,
                   R"({"cli": "x", "model": "y", "languages": {}})");
        BOOST_CHECK(host.refusal().find("no engine code") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(test_TifaGgmlInterpreter_AnalyzerCreationChecksTheFiles) {
    // A package whose engine is missing is one that cannot run, and the refusal names it.
    {
        Host host;
        host.write("cli/tifa_ggml_cli.exe");
        auto spec = host.load();
        auto made = AlignApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
        BOOST_REQUIRE(!made);
        BOOST_CHECK(made.error().rootCause().message().find("cannot find the tifa CLI") !=
                    std::string::npos);
    }
    {
        Host host;
        host.write(OTTER_TEST_FAKE_CLI, "models/absent.gguf");
        auto spec = host.load();
        auto made = AlignApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
        BOOST_REQUIRE(!made);
        BOOST_CHECK(made.error().rootCause().message().find("cannot find the tifa model") !=
                    std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(test_TifaGgmlInterpreter_AlignsAgainstTheFakeCli) {
    Host host;
    host.write();
    auto analyzer = host.open();

    // The fake tool aligns a leading gap and two words over one second: "ni" from 0.2 to 0.6 and
    // "hao" from 0.6 to 1, each with two phones. Against a one-second span the leading gap is the
    // only silence, and against a two-second span the second half is another.
    AlignApi::StartInput input;
    input.audio = tone(1.0, 3.0);
    input.lyrics = "ni hao";
    std::vector<double> reported;
    input.progress = [&reported](double value) {
        reported.push_back(value);
        return true;
    };

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), why(produced));
    auto result = produced.take();

    BOOST_CHECK_EQUAL(result->language, "cmn");
    BOOST_CHECK_EQUAL(result->scheme, "pinyin");
    BOOST_REQUIRE_EQUAL(result->words.size(), 3u);

    // The leading gap is a word of the silence label, and the words after it begin where their
    // own phones begin, not where the span does.
    BOOST_CHECK_EQUAL(result->words.front().text, "SP");
    BOOST_CHECK_CLOSE(result->words.front().start, 3.0, 1e-6);
    BOOST_CHECK_CLOSE(result->words.front().duration, 0.2, 1e-3);
    BOOST_REQUIRE_EQUAL(result->words.front().phones.size(), 1u);
    BOOST_CHECK_EQUAL(result->words[1].text, "ni");
    BOOST_CHECK_CLOSE(result->words[1].start, 3.2, 1e-3);
    BOOST_CHECK_CLOSE(result->words[1].duration, 0.4, 1e-3);
    BOOST_REQUIRE_EQUAL(result->words[1].phones.size(), 2u);
    BOOST_CHECK_EQUAL(result->words[1].phones.front().text, "n");
    BOOST_CHECK_EQUAL(result->words.back().text, "hao");
    BOOST_CHECK_CLOSE(result->words.back().start, 3.6, 1e-3);
    BOOST_REQUIRE_EQUAL(result->words.back().phones.size(), 2u);
    BOOST_CHECK_EQUAL(result->words.back().phones.front().text, "x");

    // The phones cover each word exactly, and the words cover the span.
    for (std::size_t i = 0; i < result->words.size(); ++i) {
        const auto &word = result->words[i];
        double covered = 0;
        for (const auto &phone : word.phones) {
            BOOST_CHECK_GT(phone.duration, 0.0);
            covered += phone.duration;
        }
        BOOST_CHECK_LE(std::abs(covered - word.duration), 1e-4);
        if (i != 0) {
            BOOST_CHECK_CLOSE(word.start,
                              result->words[i - 1].start + result->words[i - 1].duration, 1e-3);
        }
    }
    BOOST_CHECK_CLOSE(result->words.back().start + result->words.back().duration, 4.0, 1e-3);

    BOOST_REQUIRE_GE(reported.size(), 2u);
    BOOST_CHECK_CLOSE(reported.front(), 0.0, 1e-9);
    BOOST_CHECK_CLOSE(reported.back(), 1.0, 1e-9);

    // The stretch the decode attributed to no word becomes a word of the silence label, so the
    // result of a longer span still covers it.
    AlignApi::StartInput over;
    over.audio = tone(2.0);
    over.lyrics = "ni hao";
    auto padded = analyzer->start(over);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(padded), why(padded));
    auto paddedResult = padded.take();
    BOOST_REQUIRE_GE(paddedResult->words.size(), 4u);
    BOOST_CHECK_EQUAL(paddedResult->words.back().text, "SP");
    BOOST_CHECK_CLOSE(paddedResult->words.back().start, 1.0, 1e-3);
    BOOST_CHECK_CLOSE(paddedResult->words.back().start + paddedResult->words.back().duration, 2.0,
                      1e-3);
}

/// A stop from another thread kills the tool while the execution waits for it to finish, so the
/// execution ends as cancelled instead of waiting for a tool that never finishes. The next
/// execution runs a tool of its own.
BOOST_AUTO_TEST_CASE(test_TifaGgmlInterpreter_StopKillsAToolThatDoesNotFinish) {
    Host host;
    host.write();
    auto analyzer = host.open();

    auto input = std::make_shared<AlignApi::StartInput>();
    input->audio = tone(1.0);
    input->lyrics = "ni hao";

    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    bool succeeded = true;
    std::error_code code;
    {
        const otter::test::HangingEngine hanging(host.root / "hanging");
        auto started = analyzer->startAsync(
            input, [&](srt::Expected<std::unique_ptr<AlignApi::Result>> result) {
                std::lock_guard guard(mutex);
                succeeded = static_cast<bool>(result);
                if (!result) {
                    code = result.error().code();
                }
                finished = true;
                done.notify_all();
            });
        BOOST_REQUIRE_MESSAGE(static_cast<bool>(started), why(started));
        BOOST_REQUIRE(hanging.waitUntilHanging(std::chrono::seconds(10)));

        BOOST_CHECK(analyzer->stop());
        std::unique_lock guard(mutex);
        BOOST_REQUIRE(done.wait_for(guard, std::chrono::seconds(10), [&] { return finished; }));
    }
    BOOST_CHECK(!succeeded);
    BOOST_CHECK(code == otter::AnalysisError::Cancelled);
    BOOST_CHECK(analyzer->waitForFinished());

    AlignApi::StartInput again;
    again.audio = tone(1.0);
    again.lyrics = "ni hao";
    auto second = analyzer->start(again);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(second), why(second));
    BOOST_CHECK_EQUAL(second.take()->words.size(), 3u);
}
