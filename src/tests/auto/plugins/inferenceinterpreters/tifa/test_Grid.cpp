#include <system_error>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <tifa/Grid.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace {
    struct TemporaryDirectory {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("internal-tests-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        TemporaryDirectory() {
            std::filesystem::create_directories(path);
        }
        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
        std::filesystem::path write(const std::string &name, const std::string &text) {
            const auto result = path / name;
            std::ofstream stream(result, std::ios::binary);
            stream << text;
            BOOST_REQUIRE(stream.good());
            return result;
        }
    };
}

BOOST_AUTO_TEST_CASE(vocabulary_dictionary_and_candidate_grid_keep_pronunciation_order) {
    TemporaryDirectory temporary;
    auto vocabulary = otter::tifa::Vocabulary::read(
        temporary.write("vocabulary.json", R"({"symbols":{"en/h":1,"en/eh":2,"en/ey":3,"SP":4}})"));
    BOOST_REQUIRE(vocabulary);
    BOOST_CHECK_EQUAL(vocabulary.value().id("absent"), 0);
    auto dictionary = otter::tifa::Dictionary::load(
        temporary.write("dictionary.txt", "hello\th eh\nhello\th ey\nunknown\tz\n"));
    BOOST_REQUIRE(dictionary);
    auto candidates = dictionary.value().find("hello");
    BOOST_REQUIRE(candidates);
    BOOST_REQUIRE_EQUAL(candidates->size(), 2u);
    BOOST_CHECK((*candidates)[0] == std::vector<std::string>({"h", "eh"}));
    BOOST_CHECK(dictionary.value().find("missing") == nullptr);
    auto grid = otter::tifa::buildGrid("missing hello unknown", "en", dictionary.value(),
                                       vocabulary.value());
    BOOST_REQUIRE(grid);
    BOOST_CHECK_EQUAL(grid.value().wordCount(), 1u);
    BOOST_CHECK_EQUAL(grid.value().columns(), 2u);
    BOOST_REQUIRE_EQUAL(grid.value().readings.size(), 1u);
    BOOST_CHECK(grid.value().readings[0][0] == std::vector<std::string>({"en/h", "en/eh"}));
    BOOST_CHECK(grid.value().readings[0][1] == std::vector<std::string>({"en/h", "en/ey"}));
    auto empty = otter::tifa::buildGrid("missing", "en", dictionary.value(), vocabulary.value());
    BOOST_REQUIRE(empty);
    BOOST_CHECK_EQUAL(empty.value().wordCount(), 0u);
}

BOOST_AUTO_TEST_CASE(malformed_vocabulary_is_rejected) {
    TemporaryDirectory temporary;
    for (const auto &text :
         {"{}", R"({"symbols":{}})", R"({"symbols":{"a":-1}})", R"({"symbols":{"a":"one"}})"}) {
        auto invalid = otter::tifa::Vocabulary::read(temporary.write("bad.json", text));
        BOOST_REQUIRE(!invalid);
        BOOST_CHECK(invalid.error().code() == srt::Error::InvalidFormat);
    }
}
