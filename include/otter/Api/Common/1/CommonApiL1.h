#ifndef OTTER_API_COMMONAPIL1_H
#define OTTER_API_COMMONAPIL1_H

#include <functional>
#include <vector>

/// Common Level 1 types shared by the otter analysis interfaces.
namespace otter::Api::Common::L1 {

    /// One contiguous span of PCM, prepared by the host.
    ///
    /// otter contains no audio processing code: it does not decode, resample or slice audio. The
    /// host reads the sample rate that a module declares, supplies a span at exactly that rate,
    /// and specifies the start time of the span.
    ///
    /// The two format fields are treated differently by design. A span with more channels than
    /// the module declares is averaged down, because the caller would otherwise have to perform
    /// the same arithmetic with the same result. A span at another sample rate is rejected,
    /// because resampling changes the result, and silent resampling would conceal that the host
    /// prepared audio at the wrong rate.
    struct AudioSegment {
        /// Sample rate in hertz. The value must equal the rate that the module declares.
        int sampleRate = 0;

        /// Channel count. A count above the declared count is averaged down, and a count below
        /// it is rejected.
        int channelCount = 0;

        /// Interleaved samples. The host moves its buffer into this member, and the input owns
        /// the buffer for the duration of the execution.
        ///
        /// The count must be a whole number of frames. A partial last frame is rejected rather
        /// than dropped, because dropping it would shorten the span by a fraction of a frame
        /// without any other visible effect, and the discrepancy would appear much later as
        /// drift.
        std::vector<float> samples;

        /// Start time of this span on the host's timeline, in seconds. Results are anchored to
        /// this time.
        ///
        /// This field makes slicing a decision of the caller: a single call with the whole
        /// recording and one call per slice differ only in the value of this field.
        double startTime = 0;

        /// Returns the duration of this span in seconds.
        ///
        /// \return The duration in seconds, which is 0 if the span holds no samples, and 0 if
        /// the sample rate or the channel count is not positive.
        inline double duration() const noexcept {
            return (sampleRate > 0 && channelCount > 0)
                       ? static_cast<double>(samples.size()) /
                             (static_cast<double>(sampleRate) * channelCount)
                       : 0;
        }
    };

    /// Callback that receives execution progress in the inclusive range from 0 to 1. The callback
    /// may be empty.
    using ProgressCallback = std::function<void(double)>;

    /// Declaration of one continuous knob: whether the module honors the knob, and its accepted
    /// range.
    ///
    /// Supplying a value for a knob that the module does not honor is not an error, and the value
    /// is ignored. A host can therefore offer one settings page for variants that accept
    /// different knobs. A value outside the declared range is rejected, so that a caller that
    /// requests an unsupported setting receives an error instead of a result computed with a
    /// different setting.
    struct Knob {
        /// Indicates whether the module reads the knob.
        bool honored = false;

        /// Smallest accepted value.
        double minimum = 0;

        /// Largest accepted value.
        double maximum = 0;

        /// Value used when the caller supplies none.
        double defaultValue = 0;
    };

    /// Declaration of one integral knob, with the same fields as Knob.
    struct IntKnob {
        bool honored = false;
        int minimum = 0;
        int maximum = 0;
        int defaultValue = 0;
    };

    /// Declaration of one boolean knob, which has no range.
    struct FlagKnob {
        bool honored = false;
        bool defaultValue = false;
    };

}

#endif // OTTER_API_COMMONAPIL1_H
