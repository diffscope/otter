#define BOOST_TEST_MAIN
#include "InterpreterTestSupport.h"

BOOST_AUTO_TEST_CASE(test_GameGgmlInterpreter_TranscribesAToneWithTheRealEngine) {
    Host host;
    const auto cli = pathFromEnv("OTTER_TEST_GAME_CLI", "no game.cpp CLI; set OTTER_TEST_GAME_CLI "
                                                        "to run the engine tests");
    const auto model =
        pathFromEnv("OTTER_TEST_GAME_GGUF", "no GAME GGUF model; set "
                                            "OTTER_TEST_GAME_GGUF to run the engine tests");
    host.write(cli, model);
    auto spec = host.load();
    auto made = NoteApi::Executive::create(*spec->as<srt::InferenceSpec>());
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), why(made));
    const auto analyzer = made.take();

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

    // The notes are ordered, non-overlapping, lie inside the span, and sit on the host's timeline
    // in seconds. What the engine makes of a constant tone is its own business. Where it placed
    // them is the provider's promise.
    for (std::size_t i = 0; i < result->notes.size(); ++i) {
        const auto &note = result->notes[i];
        BOOST_CHECK_LE(note.start, 9.5);
        BOOST_CHECK_GE(note.start, 7.5);
        BOOST_CHECK_LE(note.start + note.duration, 9.5 + 1e-4);
        BOOST_CHECK_GT(note.duration, 0.0);
        // The default cutoff is above the confidence of a rest, so every note that survived is
        // one the engine called voiced.
        BOOST_CHECK_CLOSE(note.confidence, 1.0, 1e-9);
        if (i != 0) {
            BOOST_CHECK_GE(note.start,
                           result->notes[i - 1].start + result->notes[i - 1].duration - 1e-4);
        }
    }
}
