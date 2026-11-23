// The frame decode of the tifa provider, through the objects of the plugin. The decode is not part
// of what the plugin exports, and reaching it through the fixture's graphs would assert the
// fixture's arithmetic with it, so the cases call it with scores of their own making. They need no
// model, so they run in every build that has the plugin.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <tifa/Decode.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(test_Decode)

// The frame decode against the reference's own answers: the numbers below were read back from the
// reference on these very inputs rather than reasoned out here, and the run's authoritative
// readings are in docs/plans/TifaAlign.md §11.11 (the decode differencing in §11.12). A caller
// that drops zero-width tokens sees the same result either way, so only a caller that reads the
// anchors can see them.
BOOST_AUTO_TEST_CASE(test_Decode_DecodesFramesTheWayTheReferenceDoes) {
    const auto spansOf = [](std::vector<float> similarities, std::size_t frames, std::size_t tokens,
                            std::vector<std::int64_t> groups) {
        return otter::tifa::decodeFrames(similarities, frames, tokens, groups);
    };
    const auto expect = [](const std::vector<otter::tifa::Span> &actual,
                           const std::vector<std::pair<std::size_t, std::size_t>> &wanted) {
        BOOST_REQUIRE_EQUAL(actual.size(), wanted.size());
        for (std::size_t token = 0; token < wanted.size(); ++token) {
            BOOST_CHECK_MESSAGE(actual[token].start == wanted[token].first &&
                                    actual[token].end == wanted[token].second,
                                "token " << token << " spans [" << actual[token].start << ", "
                                         << actual[token].end << ") where the reference spans ["
                                         << wanted[token].first << ", " << wanted[token].second
                                         << ")");
        }
    };

    // One frame for two tokens. The frame is worth 1.2999038 on the second token against 1.0052632
    // on the first, and dropping a token costs 0.5, so sounding the first and dropping the second
    // scores 0.5052632 while dropping the first and sounding the second scores 0.7999038. The
    // second alignment wins, and the token it drops is the first one, which is what a phrase that
    // may simply end at its best scoring position gets wrong: a token left behind at the end of the
    // phrase is not free, and this phrase does not end after its first token.
    expect(spansOf(
               {
                   1.0052632F, 1.2999038F
    },
               1, 2, {0, 1}),
           {{0, 0}, {0, 1}});

    // Four frames, three tokens, a group boundary at every seam so a pause is allowed everywhere.
    // The first token fits the first two frames well and the third fits the last two, which leaves
    // the middle token nothing to take: it is dropped and holds the frame the pause before the
    // third token sits at.
    expect(spansOf(
               {
                   3.0F, -9.0F, 0.0F, 3.0F, -9.0F, 0.0F, 0.0F, -9.0F, 3.0F, 0.0F, -9.0F, 3.0F
    },
               4, 3, {0, 1, 0}),
           {{0, 2}, {2, 2}, {2, 4}});

    // A dropped token whose own position cannot hold a pause is held at the end of the span in
    // front of it instead: the seam between the first two tokens is one group's inside and no pause
    // belongs there, so the drop is anchored on the boundary behind it rather than in front.
    expect(spansOf(
               {
                   3.0F,  -9.0F, -9.0F, -9.0F, 3.0F, -9.0F, -9.0F, -9.0F, -9.0F, -9.0F,
                   -9.0F, -9.0F, -9.0F, -9.0F, 3.0F, -9.0F, -9.0F, -9.0F, -9.0F, 3.0F
    },
               5, 4, {0, 0, 1, 1}),
           {{0, 2}, {2, 2}, {3, 4}, {4, 5}});

    // A run of two dropped tokens that a pause splits. The run's first position is one group's
    // inside, so no pause belongs there, and the only pause the groups allow in reach sits between
    // the two: the reference holds the drop in front of that pause at the end of the span before
    // the run and the drop behind it at the start of the span after it, which is the one
    // arrangement where both anchors are readable at once: holding the whole run at either
    // boundary reads elsewhere.
    expect(spansOf(
               {
                   3.0F, -9.0F, -9.0F, -9.0F, 3.0F, -9.0F, -9.0F, -9.0F, -9.0F, -9.0F, -9.0F, -9.0F,
                   -9.0F, -9.0F, -9.0F, 3.0F
    },
               4, 4, {0, 0, 1, 1}),
           {{0, 2}, {2, 2}, {3, 3}, {3, 4}});
}

BOOST_AUTO_TEST_SUITE_END()
