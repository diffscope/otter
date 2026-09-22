#include <filesystem>
#include <string_view>

#include <synthrt/Support/JSON.h>

#include <otter/Support/ManifestValues.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace {

    srt::JsonValue parse(std::string_view text) {
        stdc::json::ParseError error;
        auto value = srt::JsonValue::fromJson(text, false, &error);
        BOOST_REQUIRE_EQUAL(static_cast<int>(error.code),
                            static_cast<int>(stdc::json::ParseError::NoError));
        return value;
    }

}

BOOST_AUTO_TEST_SUITE(test_ManifestValues)

BOOST_AUTO_TEST_CASE(test_ManifestValues_ResolvesPathsAgainstTheDeclaration) {
    const std::filesystem::path base = "/packages/rmvpe/analyzers/f0";

    auto relative = otter::manifest::readPath(parse("\"./rmvpe.onnx\""), base, "model");
    BOOST_REQUIRE(relative);
    BOOST_CHECK_EQUAL(relative.take(), base / "rmvpe.onnx");

    auto absolute = otter::manifest::readPath(parse("\"/opt/models/rmvpe.onnx\""), base, "model");
    BOOST_REQUIRE(absolute);
    BOOST_CHECK_EQUAL(absolute.take(), std::filesystem::path("/opt/models/rmvpe.onnx"));

    // Spec 2.4 counts \ as a separator as well, so the two spellings have to name the same file
    // whichever host reads them.
    auto backslashed =
        otter::manifest::readPath(parse(R"("an\\analyzers\\model.onnx")"), base, "model");
    auto slashed = otter::manifest::readPath(parse("\"an/analyzers/model.onnx\""), base, "model");
    BOOST_REQUIRE(backslashed);
    BOOST_REQUIRE(slashed);
    BOOST_CHECK_EQUAL(backslashed.take(), slashed.take());

    BOOST_CHECK(!otter::manifest::readPath(parse("\"\""), base, "model"));
    BOOST_CHECK(!otter::manifest::readPath(parse("16000"), base, "model"));
}

BOOST_AUTO_TEST_CASE(test_ManifestValues_ReadsNumbersInRange) {
    BOOST_CHECK_EQUAL(otter::manifest::readPositiveInt(parse("16000"), "sampleRate").take(), 16000);
    BOOST_CHECK(!otter::manifest::readPositiveInt(parse("0"), "sampleRate"));
    BOOST_CHECK(!otter::manifest::readPositiveInt(parse("-1"), "sampleRate"));
    // A rate written as a decimal is a typo, not a rate, and rounding it would hide that.
    BOOST_CHECK(!otter::manifest::readPositiveInt(parse("16000.5"), "sampleRate"));

    BOOST_CHECK_CLOSE(otter::manifest::readPositiveDouble(parse("0.01"), "interval").take(), 0.01,
                      1e-9);
    BOOST_CHECK(!otter::manifest::readPositiveDouble(parse("0"), "interval"));

    BOOST_CHECK_CLOSE(otter::manifest::readUnitDouble(parse("0.03"), "threshold").take(), 0.03,
                      1e-9);
    BOOST_CHECK(otter::manifest::readUnitDouble(parse("0"), "threshold"));
    BOOST_CHECK(otter::manifest::readUnitDouble(parse("1"), "threshold"));
    BOOST_CHECK(!otter::manifest::readUnitDouble(parse("1.5"), "threshold"));
}

BOOST_AUTO_TEST_CASE(test_ManifestValues_ReadsStringSets) {
    auto list = otter::manifest::readStringList(parse("[\"zh\", \"ja\"]"), "languages");
    BOOST_REQUIRE(list);
    const auto values = list.take();
    BOOST_REQUIRE_EQUAL(values.size(), 2u);
    BOOST_CHECK_EQUAL(values[0], "zh");
    BOOST_CHECK_EQUAL(values[1], "ja");

    BOOST_CHECK(!otter::manifest::readStringList(parse("[\"zh\", \"zh\"]"), "languages"));
    BOOST_CHECK(!otter::manifest::readStringList(parse("[\"\"]"), "languages"));
    BOOST_CHECK(!otter::manifest::readStringList(parse("\"zh\""), "languages"));
}

BOOST_AUTO_TEST_CASE(test_ManifestValues_RejectsAKeyTheContractDoesNotDefine) {
    const auto object = parse("{\"model\": \"a\", \"sampleRate\": 1}").toObject();
    BOOST_CHECK(otter::manifest::rejectUnknownKeys(object, {"model", "sampleRate"}, "here"));

    auto refused = otter::manifest::rejectUnknownKeys(object, {"model"}, "here");
    BOOST_REQUIRE(!refused);
    BOOST_CHECK(refused.error().message().find("sampleRate") != std::string::npos);
}

/// A knob declaration is three numbers with an order between them, or one boolean. Anything else
/// is a misspelling, and a misspelt knob would otherwise read as a module honoring nothing.
BOOST_AUTO_TEST_CASE(test_ManifestValues_ReadsKnobsAndRefusesTheMalformed) {
    auto knob = otter::manifest::readKnob(
        parse(R"({"minimum": 0.0, "maximum": 1.0, "default": 0.25})"), "voicingThreshold");
    BOOST_REQUIRE(knob);
    BOOST_CHECK(knob->honored);
    BOOST_CHECK_CLOSE(knob->defaultValue, 0.25, 1e-9);

    BOOST_CHECK(!otter::manifest::readKnob(parse(R"({"minimum": 0.0, "maximum": 1.0})"), "k"));
    BOOST_CHECK(!otter::manifest::readKnob(
        parse(R"({"minimum": 0.5, "maximum": 1.0, "default": 0.25})"), "k"));
    BOOST_CHECK(!otter::manifest::readKnob(
        parse(R"({"minimum": 0.0, "maximum": 1.0, "default": 0.5, "step": 0.1})"), "k"));
    BOOST_CHECK(!otter::manifest::readKnob(
        parse(R"({"minimum": "0", "maximum": 1.0, "default": 0.5})"), "k"));

    auto steps = otter::manifest::readIntKnob(
        parse(R"({"minimum": 1, "maximum": 64, "default": 8})"), "steps");
    BOOST_REQUIRE(steps);
    BOOST_CHECK_EQUAL(steps->maximum, 64);
    BOOST_CHECK(!otter::manifest::readIntKnob(
        parse(R"({"minimum": 1, "maximum": 64, "default": 8.5})"), "steps"));

    auto flag = otter::manifest::readFlagKnob(parse(R"({"default": true})"), "interpolate");
    BOOST_REQUIRE(flag);
    BOOST_CHECK(flag->honored && flag->defaultValue);
    BOOST_CHECK(!otter::manifest::readFlagKnob(parse(R"({"default": 1})"), "interpolate"));
    BOOST_CHECK(!otter::manifest::readFlagKnob(parse(R"({})"), "interpolate"));
}

BOOST_AUTO_TEST_SUITE_END()
