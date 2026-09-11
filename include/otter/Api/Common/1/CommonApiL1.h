#ifndef OTTER_API_COMMONAPIL1_H
#define OTTER_API_COMMONAPIL1_H

#include <functional>
#include <vector>

/// Common Level 1 types shared by the otter analysis interfaces.
namespace otter::Api::Common::L1 {

    /// One contiguous span of PCM, prepared by the host.
    ///
    /// otter carries no audio code: it does not decode, resample or slice. The host reads the
    /// sample rate a module declares, hands over a span at exactly that rate, and says where the
    /// span sits.
    ///
    /// The two format fields are not treated alike, deliberately. A span with more channels than
    /// the module wants is averaged down, because that is arithmetic the caller would otherwise be
    /// asked to do for no reason and the answer is the same either way. A span at another sample
    /// rate is refused, because resampling changes the answer and doing it silently would hide
    /// that the host prepared the wrong audio.
    struct AudioSegment {
        /// Sample rate in hertz. Must equal the rate the module declares.
        int sampleRate = 0;

        /// Channel count. More than the module declares is averaged down; fewer is refused.
        int channelCount = 0;

        /// Interleaved samples. The host moves its buffer in; the input owns it for the execution.
        ///
        /// The count must be a whole number of frames. A partial last frame is refused rather than
        /// dropped: dropping it shortens the span by a fraction of a frame and shifts nothing
        /// else, which surfaces much later as drift.
        std::vector<float> samples;

        /// Where this span begins on the host's timeline, in seconds. Results are anchored to it.
        ///
        /// This is what makes host side slicing a caller side decision. Calling once with
        /// everything and calling once per slice differ only in what goes here.
        double startTime = 0;

        /// Duration of this span in seconds, or 0 when it holds no samples.
        inline double duration() const noexcept {
            return (sampleRate > 0 && channelCount > 0)
                       ? static_cast<double>(samples.size()) /
                             (static_cast<double>(sampleRate) * channelCount)
                       : 0;
        }
    };

    /// Reports execution progress in the inclusive range from 0 to 1. May be empty.
    using ProgressCallback = std::function<void(double)>;

    /// Declares one continuous knob: whether this module honors it, and over what range.
    ///
    /// A knob a module does not honor is not an error to supply — it is ignored. That is what lets
    /// a host offer one settings page across variants that differ in what they accept. A value
    /// outside the declared range is a different matter and is refused: a caller that asked for a
    /// setting the module cannot honor should learn that rather than silently receive another.
    struct Knob {
        /// Indicates whether this module reads the knob at all.
        bool honored = false;

        /// Smallest accepted value.
        double minimum = 0;

        /// Largest accepted value.
        double maximum = 0;

        /// Value used when the caller supplies none.
        double defaultValue = 0;
    };

    /// Declares one integral knob.
    struct IntKnob {
        bool honored = false;
        int minimum = 0;
        int maximum = 0;
        int defaultValue = 0;
    };

    /// Declares one boolean knob.
    struct FlagKnob {
        bool honored = false;
        bool defaultValue = false;
    };

}

#endif // OTTER_API_COMMONAPIL1_H
