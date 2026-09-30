// The interpolation that the F0 providers apply across unvoiced frames, on curves small enough to
// state the expected values explicitly.

#include <cmath>
#include <cstdint>
#include <vector>

#include <otter/Support/F0Curve.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace {

    using Curve = std::vector<float>;
    using Flags = std::vector<std::uint8_t>;

}

BOOST_AUTO_TEST_SUITE(test_F0Curve)

/// A curve without a voiced frame has no anchor and is left unchanged.
BOOST_AUTO_TEST_CASE(test_F0Curve_LeavesAnAllUnvoicedCurveAlone) {
    Curve f0 = {0, 3, 0, 5};
    otter::support::interpolateUnvoiced(f0, Flags{0, 0, 0, 0});
    BOOST_CHECK(f0 == (Curve{0, 3, 0, 5}));

    Curve empty;
    otter::support::interpolateUnvoiced(empty, Flags{});
    BOOST_CHECK(empty.empty());
}

/// The frames before the first voiced frame and after the last voiced frame are held at the
/// frequency of the nearest voiced frame.
BOOST_AUTO_TEST_CASE(test_F0Curve_HoldsTheEndsFlat) {
    Curve f0 = {0, 0, 200, 0};
    otter::support::interpolateUnvoiced(f0, Flags{0, 0, 1, 0});
    BOOST_CHECK(f0 == (Curve{200, 200, 200, 200}));
}

/// A gap between two voiced frames follows the geometric path between them, so an octave over
/// two steps passes through the frequency at half the semitone distance.
BOOST_AUTO_TEST_CASE(test_F0Curve_InterpolatesInTheLogDomain) {
    Curve f0 = {100, 0, 400};
    otter::support::interpolateUnvoiced(f0, Flags{1, 0, 1});
    BOOST_CHECK_CLOSE(f0[1], 200.0f, 1e-4);
    BOOST_CHECK_EQUAL(f0[0], 100.0f);
    BOOST_CHECK_EQUAL(f0[2], 400.0f);

    Curve longer = {100, 0, 0, 0, 1600};
    otter::support::interpolateUnvoiced(longer, Flags{1, 0, 0, 0, 1});
    BOOST_CHECK_CLOSE(longer[1], 200.0f, 1e-4);
    BOOST_CHECK_CLOSE(longer[2], 400.0f, 1e-4);
    BOOST_CHECK_CLOSE(longer[3], 800.0f, 1e-4);
}

/// A voiced frame that the model reports at 0 Hz is not an anchor. A previous implementation
/// stopped the interpolation at such a frame and left the rest of the gap at the model's values.
BOOST_AUTO_TEST_CASE(test_F0Curve_SkipsAVoicedFrameAtZero) {
    Curve f0 = {100, 0, 0, 0, 1600};
    otter::support::interpolateUnvoiced(f0, Flags{1, 0, 1, 0, 1});
    // The zero is voiced, so it is kept; the gap is interpolated between 100 and 1600 around it.
    BOOST_CHECK_EQUAL(f0[2], 0.0f);
    BOOST_CHECK_CLOSE(f0[1], 200.0f, 1e-4);
    BOOST_CHECK_CLOSE(f0[3], 800.0f, 1e-4);

    // If the zero frame is the first voiced frame, the leading frames are held at the first
    // anchor instead.
    Curve leading = {0, 0, 300, 0};
    otter::support::interpolateUnvoiced(leading, Flags{0, 1, 1, 0});
    BOOST_CHECK(leading == (Curve{300, 0, 300, 300}));
}

/// Voiced frames are measurements, and interpolation never changes a voiced frame.
BOOST_AUTO_TEST_CASE(test_F0Curve_NeverChangesAVoicedFrame) {
    Curve f0 = {110, 0, 220, 0, 0, 330};
    const Flags voiced = {1, 0, 1, 0, 0, 1};
    otter::support::interpolateUnvoiced(f0, voiced);
    BOOST_CHECK_EQUAL(f0[0], 110.0f);
    BOOST_CHECK_EQUAL(f0[2], 220.0f);
    BOOST_CHECK_EQUAL(f0[5], 330.0f);
    BOOST_CHECK_GT(f0[3], 220.0f);
    BOOST_CHECK_LT(f0[4], 330.0f);
}

/// A long gap is filled in one pass. The gap is long enough that the previous rescan of the gap
/// for each frame would be noticeably slow, and the test checks that the far end of the gap is
/// still on the geometric path.
BOOST_AUTO_TEST_CASE(test_F0Curve_FillsALongGap) {
    const std::size_t length = 200000;
    Curve f0(length, 0.0f);
    Flags voiced(length, 0);
    f0.front() = 100;
    f0.back() = 200;
    voiced.front() = 1;
    voiced.back() = 1;
    otter::support::interpolateUnvoiced(f0, voiced);
    BOOST_CHECK_CLOSE(f0[length / 2], static_cast<float>(100 * std::sqrt(2.0)), 1e-3);
    BOOST_CHECK_LT(f0[length - 2], 200.0f);
    BOOST_CHECK_GT(f0[length - 2], 199.99f);
}

BOOST_AUTO_TEST_SUITE_END()
