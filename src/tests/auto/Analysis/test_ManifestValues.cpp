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

BOOST_AUTO_TEST_SUITE_END()
