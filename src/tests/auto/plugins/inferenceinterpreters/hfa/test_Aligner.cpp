#include <string>
#include <vector>

#include <hfa/Aligner.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

using otter::hfa::Aligner;
using otter::hfa::Word;

BOOST_AUTO_TEST_CASE(silence_and_language_prefixes_preserve_word_spans) {
    std::vector<Word> words{
        {"hello", 1, 1, {{"eng/h", 1, 0.5}, {"eng/eh", 1.5, 0.5}}}
    };
    BOOST_REQUIRE(Aligner::addSilence(words, 3, "SP"));
    BOOST_REQUIRE_EQUAL(words.size(), 3u);
    BOOST_CHECK_EQUAL(words[0].text, "SP");
    BOOST_CHECK_EQUAL(words[0].duration, 1);
    BOOST_CHECK_EQUAL(words[1].start, 1);
    BOOST_CHECK_EQUAL(words[2].start, 2);
    BOOST_CHECK_EQUAL(words[2].duration, 1);
    Aligner::clearLanguagePrefix(words);
    BOOST_CHECK_EQUAL(words[1].text, "hello");
    BOOST_CHECK_EQUAL(words[1].phones[0].text, "h");
    BOOST_CHECK_EQUAL(words[1].phones[1].text, "eh");
    BOOST_CHECK_EQUAL(words[0].phones[0].text, "SP");
}

BOOST_AUTO_TEST_CASE(non_speech_is_clipped_around_existing_words) {
    std::vector<Word> words{
        {"sung", 1, 1, {{"a", 1, 1}}}
    };
    BOOST_REQUIRE(Aligner::insertNonSpeech(words, {"AP", 0, 3, {{"AP", 0, 3}}}, 0.1));
    BOOST_REQUIRE_EQUAL(words.size(), 3u);
    BOOST_CHECK_EQUAL(words[0].start, 0);
    BOOST_CHECK_EQUAL(words[0].duration, 1);
    BOOST_CHECK_EQUAL(words[1].text, "sung");
    BOOST_CHECK_EQUAL(words[2].start, 2);
    BOOST_CHECK_EQUAL(words[2].duration, 1);
    auto invalid = Aligner::insertNonSpeech(words, {"empty", 0, 1, {}}, 0.1);
    BOOST_REQUIRE(!invalid);
    BOOST_CHECK(invalid.error().code() == srt::Error::InvalidArgument);
}

BOOST_AUTO_TEST_CASE(frame_count_and_decode_validation_need_no_model) {
    Aligner aligner(
        {
            {"SP", 0},
            {"a",  1}
    },
        "SP", 160, 16000);
    BOOST_CHECK_EQUAL(aligner.frameCount(1), 100);
    BOOST_CHECK_EQUAL(aligner.frameCount(0), 0);
    auto unknown = aligner.decode({}, {}, {{"unknown"}, {"word"}, {0}}, 1);
    BOOST_REQUIRE(!unknown);
    BOOST_CHECK(unknown.error().code() == srt::Error::InvalidArgument);
    auto empty = aligner.decode({}, {}, {{"a"}, {"word"}, {0}}, 1);
    BOOST_REQUIRE(!empty);
    BOOST_CHECK(empty.error().code() == srt::Error::InvalidArgument);
}
