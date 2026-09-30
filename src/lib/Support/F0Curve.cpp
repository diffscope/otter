#include <otter/Support/F0Curve.h>

#include <cmath>
#include <cstddef>

namespace otter::support {

    void interpolateUnvoiced(std::vector<float> &f0, const std::vector<std::uint8_t> &voiced) {
        const std::size_t count = f0.size() < voiced.size() ? f0.size() : voiced.size();
        const auto isAnchor = [&](std::size_t i) { return voiced[i] && f0[i] > 0; };

        // The frames before the first anchor are held at its frequency. `previous` is the last
        // anchor seen, or count if no anchor has been seen yet.
        std::size_t previous = count;
        for (std::size_t i = 0; i < count; ++i) {
            if (!isAnchor(i)) {
                continue;
            }
            if (previous == count) {
                for (std::size_t j = 0; j < i; ++j) {
                    if (!voiced[j]) {
                        f0[j] = f0[i];
                    }
                }
            } else if (i - previous > 1) {
                // The direct form of the geometric interpolation, so that each frame depends on
                // the two anchors only and not on the frames filled before it.
                const double from = f0[previous];
                const double ratio = std::log(static_cast<double>(f0[i]) / from);
                const double span = static_cast<double>(i - previous);
                for (std::size_t j = previous + 1; j < i; ++j) {
                    if (!voiced[j]) {
                        f0[j] = static_cast<float>(
                            from * std::exp(ratio * static_cast<double>(j - previous) / span));
                    }
                }
            }
            previous = i;
        }
        if (previous == count) {
            return;
        }
        for (std::size_t j = previous + 1; j < count; ++j) {
            if (!voiced[j]) {
                f0[j] = f0[previous];
            }
        }
    }

}
