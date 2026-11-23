#include <string_view>

#include <synthrt/Support/Json.h>

#include <otter/Api/Common/1/CommonApiL1.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace {

    namespace Common = otter::Api::Common::L1;

    srt::JsonValue parse(std::string_view text) {
        stdc::json::ParseError error;
        auto value = srt::JsonValue::fromJson(text, false, &error);
        BOOST_REQUIRE_EQUAL(static_cast<int>(error.code),
                            static_cast<int>(stdc::json::ParseError::NoError));
        return value;
    }

}

// A knob declaration consists of three ordered numbers or of one boolean. Any other form is a
// misspelling, and an accepted misspelled knob would make the module appear to honor no knob.
BOOST_AUTO_TEST_CASE(knob_declarations_are_read_and_malformed_ones_are_refused) {
    auto knob = Common::Knob::read(
        parse(R"({"minimum": 0.0, "maximum": 1.0, "default": 0.25})"), "voicingThreshold");
    BOOST_REQUIRE(knob);
    BOOST_CHECK(knob->honored);
    BOOST_CHECK_CLOSE(knob->defaultValue, 0.25, 1e-9);

    BOOST_CHECK(!Common::Knob::read(parse(R"({"minimum": 0.0, "maximum": 1.0})"), "k"));
    BOOST_CHECK(!Common::Knob::read(
        parse(R"({"minimum": 0.5, "maximum": 1.0, "default": 0.25})"), "k"));
    BOOST_CHECK(!Common::Knob::read(
        parse(R"({"minimum": 0.0, "maximum": 1.0, "default": 0.5, "step": 0.1})"), "k"));
    BOOST_CHECK(!Common::Knob::read(
        parse(R"({"minimum": "0", "maximum": 1.0, "default": 0.5})"), "k"));

    auto steps =
        Common::IntKnob::read(parse(R"({"minimum": 1, "maximum": 64, "default": 8})"), "steps");
    BOOST_REQUIRE(steps);
    BOOST_CHECK_EQUAL(steps->maximum, 64);
    BOOST_CHECK(!Common::IntKnob::read(
        parse(R"({"minimum": 1, "maximum": 64, "default": 8.5})"), "steps"));

    auto flag = Common::FlagKnob::read(parse(R"({"default": true})"), "interpolate");
    BOOST_REQUIRE(flag);
    BOOST_CHECK(flag->honored && flag->defaultValue);
    BOOST_CHECK(!Common::FlagKnob::read(parse(R"({"default": 1})"), "interpolate"));
    BOOST_CHECK(!Common::FlagKnob::read(parse(R"({})"), "interpolate"));
}