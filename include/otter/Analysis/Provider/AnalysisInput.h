#ifndef OTTER_ANALYSIS_PROVIDER_ANALYSISINPUT_H
#define OTTER_ANALYSIS_PROVIDER_ANALYSISINPUT_H

#include <cstddef>
#include <utility>
#include <vector>

#include <synthrt/Support/Expected.h>

#include <otter/Api/Common/1/CommonApiL1.h>
#include <otter/otter_global.h>

namespace otter {

    /// Single-channel samples that one execution supplies to its model.
    ///
    /// A single-channel span is passed through as a view of the caller's buffer rather than
    /// copied, because one minute of audio at 44.1 kHz occupies about ten megabytes. A span with
    /// more channels is averaged into a buffer owned by this object. A view remains valid for the
    /// lifetime of the \c AudioSegment from which it was prepared, which for a body covers the
    /// whole execution.
    class PreparedSamples {
    public:
        /// Returns a view of \a samples, which must outlive the result and every copy of it.
        static PreparedSamples view(const std::vector<float> &samples) {
            PreparedSamples result;
            result.m_viewed = &samples;
            return result;
        }

        /// Returns an object that owns \a samples.
        static PreparedSamples own(std::vector<float> samples) {
            PreparedSamples result;
            result.m_owned = std::move(samples);
            return result;
        }

        /// Returns a pointer to the first sample.
        const float *data() const noexcept {
            return m_viewed ? m_viewed->data() : m_owned.data();
        }

        /// Returns the number of samples.
        std::size_t size() const noexcept {
            return m_viewed ? m_viewed->size() : m_owned.size();
        }

        /// Returns whether the object contains no samples.
        bool empty() const noexcept {
            return size() == 0;
        }

        /// Returns a pointer to the first sample.
        const float *begin() const noexcept {
            return data();
        }

        /// Returns a pointer past the last sample.
        const float *end() const noexcept {
            return data() + size();
        }

        /// Checks one span against the audio format of a model and returns the samples to supply
        /// to the model.
        ///
        /// The two format fields are treated differently by design. Channels are averaged to one
        /// channel, because the caller would otherwise have to perform the same arithmetic with the
        /// same result. The sample rate is never converted, because resampling changes the result,
        /// and silent resampling would conceal that the host prepared audio at the wrong rate.
        ///
        /// \a sampleRate and \a channelCount are the values that the model declares, and
        /// \a maxSegmentDuration is the declared longest span in seconds, 0 indicating no limit. A
        /// mono span is returned as a view of the samples of \a audio. \a audio must therefore
        /// outlive the result.
        ///
        /// \return The prepared samples. An error with \c FeatureNotSupported if \a channelCount
        /// is not 1. An error with \c InvalidArgument if the span has another sample rate,
        /// declares no channels, holds no samples, is not a whole number of frames or is longer
        /// than \a maxSegmentDuration.
        OTTER_EXPORT static srt::Expected<PreparedSamples>
            prepare(const Api::Common::L1::AudioSegment &audio, int sampleRate, int channelCount,
                    double maxSegmentDuration);

    private:
        PreparedSamples() = default;

        const std::vector<float> *m_viewed = nullptr;
        std::vector<float> m_owned;
    };

}

#endif // OTTER_ANALYSIS_PROVIDER_ANALYSISINPUT_H
