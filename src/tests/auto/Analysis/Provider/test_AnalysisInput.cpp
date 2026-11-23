#include <optional>
#include <utility>
#include <vector>

#include <otter/Analysis/Provider/AnalysisInput.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace Common = otter::Api::Common::L1;

BOOST_AUTO_TEST_CASE(prepared_samples_preserve_view_and_owned_buffer_lifetimes) {
    std::vector<float> source{1, 2, 3};
    auto view = otter::PreparedSamples::view(source);
    auto copy = view;
    BOOST_CHECK(view.data() == source.data());
    BOOST_CHECK(copy.data() == source.data());
    source[0] = 4;
    BOOST_CHECK_EQUAL(copy.data()[0], 4);
    auto owned = otter::PreparedSamples::own({5, 6});
    auto ownedCopy = owned;
    BOOST_CHECK(owned.data() != ownedCopy.data());
    auto moved = std::move(owned);
    BOOST_CHECK_EQUAL(moved.size(), 2u);
    BOOST_CHECK_EQUAL(moved.data()[1], 6);
    BOOST_CHECK_EQUAL(ownedCopy.data()[0], 5);
}

BOOST_AUTO_TEST_CASE(mono_is_borrowed_stereo_is_averaged_and_invalid_audio_is_rejected) {
    Common::AudioSegment audio;
    audio.sampleRate = 100;
    audio.channelCount = 1;
    audio.samples = {1, 3, 5, 7};
    auto mono = otter::PreparedSamples::prepare(audio, 100, 1, 0);
    BOOST_REQUIRE(mono);
    BOOST_CHECK(mono.value().data() == audio.samples.data());
    audio.channelCount = 2;
    auto stereo = otter::PreparedSamples::prepare(audio, 100, 1, 0);
    BOOST_REQUIRE(stereo);
    BOOST_REQUIRE_EQUAL(stereo.value().size(), 2u);
    BOOST_CHECK_EQUAL(stereo.value().data()[0], 2);
    BOOST_CHECK_EQUAL(stereo.value().data()[1], 6);
    BOOST_CHECK(!otter::PreparedSamples::prepare(audio, 200, 1, 0));
    BOOST_CHECK(!otter::PreparedSamples::prepare(audio, 100, 2, 0));
    BOOST_CHECK(!otter::PreparedSamples::prepare(audio, 100, 1, 0.001));
    audio.samples.pop_back();
    BOOST_CHECK(!otter::PreparedSamples::prepare(audio, 100, 1, 0));
}

BOOST_AUTO_TEST_CASE(knobs_distinguish_ignored_default_and_supplied_values) {
    Common::Knob continuous{true, 0, 1, 0.5};
    auto defaultValue = continuous.choose(std::optional<double>(), 9.0, "threshold");
    BOOST_REQUIRE(defaultValue);
    BOOST_CHECK_EQUAL(defaultValue.value(), 0.5);
    BOOST_CHECK(!continuous.choose(std::optional<double>(1.1), 9.0, "threshold"));
    continuous.honored = false;
    auto ignored = continuous.choose(std::optional<double>(1.1), 9.0, "threshold");
    BOOST_REQUIRE(ignored);
    BOOST_CHECK_EQUAL(ignored.value(), 9);
    Common::IntKnob integral{true, 1, 4, 2};
    auto value = integral.choose(std::optional<int>(4), 9, "steps");
    BOOST_REQUIRE(value);
    BOOST_CHECK_EQUAL(value.value(), 4);
    BOOST_CHECK(!integral.choose(std::optional<int>(0), 9, "steps"));
    Common::FlagKnob flag{true, true};
    BOOST_CHECK(flag.choose(std::optional<bool>(), false));
    BOOST_CHECK(!flag.choose(std::optional<bool>(false), true));
}
