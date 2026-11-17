#include <vector>

#include <analysisutil/KnownNotes.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(notes_use_absolute_timeline_and_tolerate_roundoff) {
    otter::Api::Common::L1::AudioSegment audio;
    audio.startTime = 10;
    audio.sampleRate = 100;
    audio.channelCount = 1;
    audio.samples.resize(200);
    BOOST_CHECK(otter::support::checkKnownNotes({}, audio));
    BOOST_CHECK(otter::support::checkKnownNotes(
        {
            {10, 1},
            {11, 1}
    },
        audio));
    BOOST_CHECK(otter::support::checkKnownNotes(
        {
            {10 - 1e-10, 1        },
            {11,         1 + 1e-10}
    },
        audio));
    for (const auto &notes : std::vector<std::vector<otter::Api::Note::L1::KnownNote>>{
             {{0, 1}},
             {{10, 0}},
             {{10, -1}},
             {{10, 3}},
             {{10, 1.1}, {11, 0.5}},
             {{11, 0.5}, {10, 0.5}}
    }) {
        auto invalid = otter::support::checkKnownNotes(notes, audio);
        BOOST_REQUIRE(!invalid);
        BOOST_CHECK(invalid.error().code() == srt::Error::InvalidArgument);
    }
}
