#include <cstdint>
#include <vector>

#include <tifa/Scoring.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(segment_capacity_handles_explicit_and_implicit_padding) {
    otter::tifa::Fragments table;
    table.capacity = {0, 2, 3};
    BOOST_CHECK(otter::tifa::segmentSlots(table) == std::vector<int64_t>({0, 2, 3, 0}));
    table.capacity = {2, 3};
    BOOST_CHECK(otter::tifa::segmentSlots(table) == std::vector<int64_t>({0, 2, 3}));
}

BOOST_AUTO_TEST_CASE(malformed_fragment_shapes_are_rejected_before_scoring) {
    otter::tifa::Fragments table;
    auto invalid = otter::tifa::chooseCandidates(table, {{true}});
    BOOST_REQUIRE(!invalid);
    BOOST_CHECK(invalid.error().code() == srt::Error::InvalidFormat);
    table.fragments = 2;
    table.candidates = 1;
    table.descriptors = {{{0, 0}}, {{1, 1}}};
    table.lengths = {0, 1};
    table.costs.assign(4, 0);
    table.tails.assign(4, 0);
    table.capacity = {0, 1};
    auto valid = otter::tifa::chooseCandidates(table, {{true}});
    BOOST_REQUIRE(valid);
    BOOST_CHECK(valid.value() == std::vector<int64_t>({1}));
    table.descriptors[1][0] = 2;
    auto outOfRange = otter::tifa::chooseCandidates(table, {{true}});
    BOOST_REQUIRE(!outOfRange);
    BOOST_CHECK(outOfRange.error().code() == srt::Error::InvalidFormat);
}
