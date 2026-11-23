#include <system_error>
#define BOOST_TEST_MAIN
#include "InterpreterTestSupport.h"

BOOST_AUTO_TEST_CASE(test_GameGgmlInterpreter_ReportsWhatTheDeclarationExports) {
    Host host;
    host.write();
    auto spec = host.load();
    auto schema = spec->exports()->as<NoteApi::Exports>();
    BOOST_REQUIRE(schema != nullptr);

    BOOST_CHECK_EQUAL(schema->sampleRate, RATE);
    BOOST_CHECK_EQUAL(schema->channelCount, 1);
    BOOST_CHECK_CLOSE(schema->maxSegmentDuration, 30.0, 1e-9);
    BOOST_CHECK(!schema->supportsKnownNotes);
    BOOST_REQUIRE_EQUAL(schema->languages.size(), 1u);
    BOOST_CHECK_EQUAL(schema->languages.front(), "cmn");
    BOOST_CHECK_EQUAL(schema->defaultLanguage, "cmn");
    BOOST_CHECK(schema->steps.honored);
    BOOST_CHECK_EQUAL(schema->steps.defaultValue, 8);
    BOOST_CHECK_CLOSE(schema->boundaryRadius.defaultValue, 0.02, 1e-9);
    BOOST_CHECK_CLOSE(schema->notePresenceCutoff.defaultValue, 0.5, 1e-9);
}

BOOST_AUTO_TEST_CASE(test_GameGgmlInterpreter_RefusesTheDeclarationsItCannotHonor) {
    // The serve protocol runs no alignment model, so a declaration that promises the alignment
    // path is refused at load, where the host can still pick another package.
    {
        Host host;
        host.writeWiring(R"({"cli": "x", "model": "y", "languages": {"cmn": 1}})",
                         R"({
                             "sampleRate": 44100,
                             "channelCount": 1,
                             "maxSegmentDuration": 30,
                             "languages": ["cmn"],
                             "defaultLanguage": "cmn",
                             "supportsKnownNotes": true
                         })");
        BOOST_CHECK(host.refusal().find("cannot be conditioned on known notes") !=
                    std::string::npos);
    }
    // A language the exports promise but the configuration does not number would leave the
    // execution without a number to pass to the model.
    {
        Host host;
        host.writeWiring(R"({"cli": "x", "model": "y", "languages": {"yue": 1}})");
        BOOST_CHECK(host.refusal().find("no numbering") != std::string::npos);
    }
    // A configuration key outside the variant's own vocabulary is a misspelling, and an accepted
    // misspelling would leave the intended value silently absent.
    {
        Host host;
        host.writeWiring(
            R"({"cli": "x", "model": "y", "languages": {"cmn": 1}, "scheduleStart": 0})");
        BOOST_CHECK(host.refusal().find("scheduleStart") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(test_GameGgmlInterpreter_AnalyzerCreationChecksTheFiles) {
    // A package whose engine is missing is one that cannot run, and the refusal names it.
    {
        Host host;
        host.write("cli/game_ggml_cli.exe");
        auto spec = host.load();
        auto made = NoteApi::Executive::create(*spec->as<srt::InferenceSpec>());
        BOOST_REQUIRE(!made);
        BOOST_CHECK(made.error().rootCause().message().find("cannot find the game CLI") !=
                    std::string::npos);
    }
    {
        Host host;
        host.write(OTTER_TEST_FAKE_CLI, "models/absent.gguf");
        auto spec = host.load();
        auto made = NoteApi::Executive::create(*spec->as<srt::InferenceSpec>());
        BOOST_REQUIRE(!made);
        BOOST_CHECK(made.error().rootCause().message().find("cannot find the game model") !=
                    std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(test_GameGgmlInterpreter_TranscribesAgainstTheFakeCli) {
    Host host;
    host.write();
    auto analyzer = host.open();

    NoteApi::StartInput input;
    input.audio = tone(2.0, 7.5);
    std::vector<double> reported;
    input.progress = [&reported](double value) {
        reported.push_back(value);
        return true;
    };

    auto produced = analyzer->start(input);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(produced), why(produced));
    auto result = produced.take();

    // The fake engine answers with one voiced note of half a second and one rest. The default
    // cutoff is above the confidence of a rest, so the transcription keeps the first only.
    BOOST_REQUIRE_EQUAL(result->notes.size(), 1u);
    BOOST_CHECK_EQUAL(result->notes.front().key, 60);
    BOOST_CHECK_CLOSE(result->notes.front().start, 7.5, 1e-6);
    BOOST_CHECK_CLOSE(result->notes.front().duration, 0.5, 1e-6);
    BOOST_CHECK_CLOSE(result->notes.front().confidence, 1.0, 1e-9);

    // The engine call is monolithic, so the execution reports its progress at its start and its
    // end and nowhere between.
    BOOST_REQUIRE_GE(reported.size(), 2u);
    BOOST_CHECK_CLOSE(reported.front(), 0.0, 1e-9);
    BOOST_CHECK_CLOSE(reported.back(), 1.0, 1e-9);

    // A second execution reuses the same analyzer, whose engine process is still warm.
    NoteApi::StartInput again;
    again.audio = tone(1.0, 1.0);
    auto second = analyzer->start(again);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(second), why(second));
    BOOST_REQUIRE_EQUAL(second.take()->notes.size(), 1u);
}

BOOST_AUTO_TEST_CASE(test_GameGgmlInterpreter_RefusesKnownNotes) {
    Host host;
    host.write();
    auto analyzer = host.open();

    NoteApi::StartInput input;
    input.audio = tone(2.0);
    input.knownNotes = {
        {0.5, 0.5}
    };
    auto refused = analyzer->start(input);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().rootCause().message().find(
                    "cannot be conditioned on known notes") != std::string::npos);
}

// A stop from another thread kills the engine while the execution waits for its answer, so the
// execution ends as canceled instead of waiting for an answer that never comes. The next
// execution starts an engine of its own.
BOOST_AUTO_TEST_CASE(test_GameGgmlInterpreter_StopKillsAnEngineThatDoesNotAnswer) {
    Host host;
    host.write();
    auto analyzer = host.open();

    auto input = std::make_shared<NoteApi::StartInput>();
    input->audio = tone(1.0);

    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    bool succeeded = true;
    std::error_code code;
    {
        const otter::test::HangingEngine hanging(host.root / "hanging");
        auto started = analyzer->startAsync(
            input, [&](srt::Expected<std::unique_ptr<NoteApi::Result>> result) {
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

    NoteApi::StartInput again;
    again.audio = tone(1.0);
    auto second = analyzer->start(again);
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(second), why(second));
    BOOST_CHECK_EQUAL(second.take()->notes.size(), 1u);
}
