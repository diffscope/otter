#ifndef OTTER_API_COMMON_1_COMMONAPIL1_H
#define OTTER_API_COMMON_1_COMMONAPIL1_H

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include <synthrt/Support/Expected.h>
#include <synthrt/Support/Json.h>

#include <otter/otter_global.h>

/// Common Level 1 types shared by the otter analysis interfaces.
///
/// The level in the directory name follows the interfaces that use these types: all three
/// contracts take them at Level 1, so a later level that still needs them includes this file
/// rather than carrying a copy.
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
    ///
    /// The callback reports whether the execution continues. A callback that returns false
    /// requests that the execution stop, and the body honors the request the way it honors any
    /// other stop request, by polling cancelled() and by returning an error whose code is
    /// \c AnalysisError::Cancelled. An execution that had already produced its result when the
    /// request arrived keeps that result.
    using ProgressCallback = std::function<bool(double)>;

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

        /// Reads one declared knob. \a what names the knob in error messages.
        ///
        /// The declaration is the object formed by the keys \c minimum, \c maximum and
        /// \c default, and a key outside them is rejected.
        ///
        /// \return The declaration, or an \c InvalidFormat error that describes the first invalid
        /// value.
        OTTER_EXPORT static srt::Expected<Knob> read(const srt::JsonValue &value,
                                                     std::string_view what);

        /// Returns the value to use for this knob, or an error if \a given is outside the declared
        /// range.
        ///
        /// \a fallback is the constant of the implementation that the knob replaces. A module
        /// that does not honor the knob ignores \a given, because the declaration excludes the
        /// knob from the caller's control.
        ///
        /// \return \a fallback if the knob is not honored. Otherwise the declared default if
        /// \a given is empty, \a given if it lies in the declared range, and an
        /// \c InvalidArgument error if it does not.
        OTTER_EXPORT srt::Expected<double> choose(const std::optional<double> &given,
                                                  double fallback, std::string_view what) const;
    };

    /// Declaration of one integral knob, with the same fields as Knob.
    struct IntKnob {
        bool honored = false;
        int minimum = 0;
        int maximum = 0;
        int defaultValue = 0;

        /// Reads one declared integral knob. \a what names the knob in error messages.
        OTTER_EXPORT static srt::Expected<IntKnob> read(const srt::JsonValue &value,
                                                        std::string_view what);

        /// Returns the value to use for this knob, as \c Knob::choose does.
        OTTER_EXPORT srt::Expected<int> choose(const std::optional<int> &given, int fallback,
                                               std::string_view what) const;
    };

    /// Declaration of one boolean knob, which has no range.
    struct FlagKnob {
        bool honored = false;
        bool defaultValue = false;

        /// Reads one declared boolean knob, which holds only the key \c default.
        ///
        /// \a what names the knob in error messages.
        OTTER_EXPORT static srt::Expected<FlagKnob> read(const srt::JsonValue &value,
                                                         std::string_view what);

        /// Returns the value to use for this knob, which has no range to check.
        ///
        /// \return \a fallback if the knob is not honored. Otherwise the declared default if
        /// \a given is empty, and \a given otherwise.
        OTTER_EXPORT bool choose(const std::optional<bool> &given, bool fallback) const;
    };

}

#endif // OTTER_API_COMMON_1_COMMONAPIL1_H
