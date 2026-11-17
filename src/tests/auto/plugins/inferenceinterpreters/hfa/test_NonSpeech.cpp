#include <vector>

#include <hfa/NonSpeech.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(class_threshold_duration_and_validation) {
    otter::hfa::NonSpeech decoder({"None", "AP"}, 160, 16000);
    decoder.setThreshold(0.5);
    decoder.setMinimumDuration(0.01);
    const std::vector<std::vector<float>> logits{std::vector<float>(20, -10),
                                                 std::vector<float>(20, 10)};
    auto spans = decoder.decode(logits, 0.2, {"AP"});
    BOOST_REQUIRE(spans);
    BOOST_REQUIRE_EQUAL(spans.value().size(), 1u);
    BOOST_CHECK_EQUAL(spans.value()[0].text, "AP");
    BOOST_CHECK_SMALL(spans.value()[0].start, 1e-9);
    BOOST_CHECK_GT(spans.value()[0].duration, 0);
    auto none = decoder.decode(logits, 0.2, {});
    BOOST_REQUIRE(none);
    BOOST_CHECK(none.value().empty());
    auto unknown = decoder.decode(logits, 0.2, {"unknown"});
    BOOST_REQUIRE(!unknown);
    BOOST_CHECK(unknown.error().code() == srt::Error::InvalidArgument);
    auto empty = decoder.decode({}, 0.2, {"AP"});
    BOOST_REQUIRE(!empty);
    BOOST_CHECK(empty.error().code() == srt::Error::InvalidArgument);
    decoder.setMinimumDuration(1);
    auto filtered = decoder.decode(logits, 0.2, {"AP"});
    BOOST_REQUIRE(filtered);
    BOOST_CHECK(filtered.value().empty());
}
