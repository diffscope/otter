// The pronunciation dictionary of the hfa provider, through the objects of the plugin. The phrase
// it builds is what the aligner reads, so the separators and the word indices are checked here,
// where no model is needed to see them.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <hfa/G2p.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace fs = std::filesystem;

using otter::hfa::G2p;
using otter::hfa::Phrase;

namespace {

    // Writes \a content to a scratch file byte for byte and returns its path.
    fs::path writeDictionary(const std::string &name, const std::string &content) {
        const auto path = fs::temp_directory_path() / ("otter-hfa-g2p-" + name + ".txt");
        std::ofstream(path, std::ios::binary) << content;
        return path;
    }

    G2p load(const std::string &name, const std::string &content) {
        const auto path = writeDictionary(name, content);
        auto dictionary = G2p::load(path);
        fs::remove(path);
        BOOST_REQUIRE_MESSAGE(dictionary.hasValue(),
                              (dictionary ? std::string() : dictionary.error().message()));
        return dictionary.take();
    }

    void checkPhrase(const Phrase &phrase, const std::vector<std::string> &phonemes,
                     const std::vector<int> &phonemeToWord, const std::vector<std::string> &words) {
        BOOST_CHECK_EQUAL_COLLECTIONS(phrase.phonemes.begin(), phrase.phonemes.end(),
                                      phonemes.begin(), phonemes.end());
        BOOST_CHECK_EQUAL_COLLECTIONS(phrase.phonemeToWord.begin(), phrase.phonemeToWord.end(),
                                      phonemeToWord.begin(), phonemeToWord.end());
        BOOST_CHECK_EQUAL_COLLECTIONS(phrase.words.begin(), phrase.words.end(), words.begin(),
                                      words.end());
    }

}

BOOST_AUTO_TEST_SUITE(test_G2p)

// Every word is framed by the separator, once between two words, and its phonemes carry its
// index in the resolved words.
BOOST_AUTO_TEST_CASE(test_G2p_FramesEveryWordWithTheSeparator) {
    const auto dictionary = load("frame", "ni\tn i\nhao\th ao\n");
    checkPhrase(dictionary.split("ni hao", "SP"), {"SP", "n", "i", "SP", "h", "ao", "SP"},
                {-1, 0, 0, -1, 1, 1, -1}, {"ni", "hao"});
    // Any whitespace separates words
    checkPhrase(dictionary.split("  ni\t\nhao ", "SP"), {"SP", "n", "i", "SP", "h", "ao", "SP"},
                {-1, 0, 0, -1, 1, 1, -1}, {"ni", "hao"});
    checkPhrase(dictionary.split("", "SP"), {"SP"}, {-1}, {});
}

// A word the dictionary lacks is left out of the phrase, and the caller sees it missing from the
// words.
BOOST_AUTO_TEST_CASE(test_G2p_LeavesOutWordsItCannotResolve) {
    const auto dictionary = load("missing", "ni\tn i\n");
    checkPhrase(dictionary.split("wo ni ta", "SP"), {"SP", "n", "i", "SP"}, {-1, 0, 0, -1}, {"ni"});
}

// A separator at either end of an entry would read as the gap between two words, so it is
// dropped, while one inside the entry stays a phoneme of the word. A word left with no phoneme
// stays a word and adds no separator.
BOOST_AUTO_TEST_CASE(test_G2p_DropsTheSeparatorAtTheEndsOfAnEntry) {
    const auto dictionary = load("ends", "a\tSP a SP\n"
                                         "b\tb SP b\n"
                                         "breath\tSP\n");
    checkPhrase(dictionary.split("a b", "SP"), {"SP", "a", "SP", "b", "SP", "b", "SP"},
                {-1, 0, -1, 1, 1, 1, -1}, {"a", "b"});
    checkPhrase(dictionary.split("a breath b", "SP"), {"SP", "a", "SP", "b", "SP", "b", "SP"},
                {-1, 0, -1, 2, 2, 2, -1}, {"a", "breath", "b"});
    // The label of the declaration decides what a separator is: under another one, the SP of the
    // entry is a phoneme of the word
    checkPhrase(dictionary.split("breath", "AP"), {"AP", "SP", "AP"}, {-1, 0, -1}, {"breath"});
}

// CRLF and LF dictionaries make the same table, and blank lines are skipped.
BOOST_AUTO_TEST_CASE(test_G2p_ReadsCrlfLikeLf) {
    const auto dictionary = load("crlf", "ni\tn i\r\n\r\nhao\th  ao \r\n");
    checkPhrase(dictionary.split("ni hao", "SP"), {"SP", "n", "i", "SP", "h", "ao", "SP"},
                {-1, 0, 0, -1, 1, 1, -1}, {"ni", "hao"});
}

BOOST_AUTO_TEST_CASE(test_G2p_RefusesWhatItCannotRead) {
    const auto path = writeDictionary("no-tab", "ni\tn i\nhao h ao\n");
    auto refused = G2p::load(path);
    fs::remove(path);
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().code() == srt::Error::InvalidFormat);
    BOOST_CHECK_MESSAGE(refused.error().message().find("\"hao h ao\" separates no phonemes") !=
                            std::string::npos,
                        refused.error().message());

    auto absent = G2p::load(fs::temp_directory_path() / "otter-hfa-g2p-absent" / "dict.txt");
    BOOST_REQUIRE(!absent);
    BOOST_CHECK(absent.error().code() == srt::Error::FileNotOpen);
}

BOOST_AUTO_TEST_SUITE_END()
