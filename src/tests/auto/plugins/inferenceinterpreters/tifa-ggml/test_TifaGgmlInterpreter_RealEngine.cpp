#define BOOST_TEST_MAIN
#include "InterpreterTestSupport.h"

BOOST_AUTO_TEST_CASE(test_TifaGgmlInterpreter_AlignsLyricsWithTheRealEngine) {
    Host host;
    const auto cli = pathFromEnv("OTTER_TEST_TIFA_CLI", "no tifa.cpp CLI; set OTTER_TEST_TIFA_CLI "
                                                        "to run the engine tests");
    const auto model =
        pathFromEnv("OTTER_TEST_TIFA_GGUF", "no TIFA GGUF model; set "
                                            "OTTER_TEST_TIFA_GGUF to run the engine tests");
    const auto dicts =
        pathFromEnv("OTTER_TEST_TIFA_DICTS", "no TIFA G2P dictionaries; set "
                                             "OTTER_TEST_TIFA_DICTS to run the engine tests");
    host.write(cli, model, dicts);
    auto spec = host.load();
    auto made = AlignApi::createAnalyzer(*spec->as<srt::InferenceSpec>());
    BOOST_REQUIRE_MESSAGE(static_cast<bool>(made), why(made));
    const auto analyzer = made.take();

    AlignApi::AlignStartInput input;
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
    BOOST_REQUIRE_GE(result->words.size(), 1u);

    // The words cover the span: the declaration carries a silence label, so the gaps the decode
    // attributed to no word are words of their own, and the result runs from the start of the
    // audio to its end without a hole.
    BOOST_CHECK_CLOSE(result->words.front().start, 3.0, 1e-6);
    for (std::size_t i = 0; i < result->words.size(); ++i) {
        const auto &word = result->words[i];
        BOOST_CHECK_GT(word.duration, 0.0);
        BOOST_REQUIRE_GE(word.phones.size(), 1u);
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
}
