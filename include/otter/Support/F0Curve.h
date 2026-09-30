#ifndef OTTER_F0CURVE_H
#define OTTER_F0CURVE_H

#include <cstdint>
#include <vector>

/// Curve arithmetic shared by the F0 providers, so that the stub and the shipped variant fill
/// unvoiced frames identically. This header is not part of the public API; see ManifestValues.h.
namespace otter::support {

    /// Interpolates \a f0 across its unvoiced frames, in the log domain.
    ///
    /// \a voiced marks the frames that carry a measurement, one entry per frame of \a f0. The
    /// anchors are the voiced frames with a frequency above zero. A voiced frame at 0 Hz has no
    /// logarithm, so it is neither an anchor nor filled, and the unvoiced frames around it are
    /// interpolated between the anchors on either side. An unvoiced frame between two anchors
    /// receives the geometric interpolation of the two anchors by frame distance. An unvoiced
    /// frame before the first anchor or after the last anchor receives the frequency of that
    /// anchor. A curve without an anchor is left unchanged. Voiced frames are never changed.
    ///
    /// The function runs in one pass over the curve.
    void interpolateUnvoiced(std::vector<float> &f0, const std::vector<std::uint8_t> &voiced);

}

#endif // OTTER_F0CURVE_H
