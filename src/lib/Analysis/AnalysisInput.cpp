#include <otter/Analysis/AnalysisInput.h>

#include <string>

namespace otter {

    srt::Expected<std::vector<float>> prepareSamples(const Api::Common::L1::AudioSegment &audio,
                                                     int sampleRate, int channelCount,
                                                     double maxSegmentDuration) {
        if (channelCount != 1) {
            // Nothing here can widen a span, so a model wanting more than one channel would have
            // to be fed something this cannot produce. Refusing at the first execution beats
            // handing it silently averaged audio.
            return srt::Error(srt::Error::FeatureNotSupported,
                              "this build feeds models one channel, and this one declares " +
                                  std::to_string(channelCount));
        }
        if (audio.sampleRate != sampleRate) {
            return srt::Error(srt::Error::InvalidArgument,
                              "this model needs " + std::to_string(sampleRate) +
                                  " Hz and was given " + std::to_string(audio.sampleRate) +
                                  " Hz; the host resamples, this analyzer does not");
        }
        if (audio.channelCount < 1) {
            return srt::Error(srt::Error::InvalidArgument, "the audio declares no channels");
        }
        if (audio.samples.empty()) {
            return srt::Error(srt::Error::InvalidArgument, "the audio holds no samples");
        }
        if (audio.samples.size() % static_cast<std::size_t>(audio.channelCount) != 0) {
            // A truncated last frame would be dropped by the division below, which shortens the
            // span by a sample and shifts nothing else — the kind of thing that shows up much
            // later as a fraction of a frame of drift.
            return srt::Error(srt::Error::InvalidArgument,
                              "the audio holds " + std::to_string(audio.samples.size()) +
                                  " samples, which is not a whole number of " +
                                  std::to_string(audio.channelCount) + " channel frames");
        }
        if (maxSegmentDuration > 0 && audio.duration() > maxSegmentDuration) {
            return srt::Error(srt::Error::InvalidArgument, "this model accepts at most " +
                                                               std::to_string(maxSegmentDuration) +
                                                               " seconds in one execution");
        }

        if (audio.channelCount == 1) {
            return audio.samples;
        }
        const auto frames = audio.samples.size() / static_cast<std::size_t>(audio.channelCount);
        std::vector<float> mono(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            float sum = 0;
            for (int c = 0; c < audio.channelCount; ++c) {
                sum += audio.samples[i * audio.channelCount + c];
            }
            mono[i] = sum / static_cast<float>(audio.channelCount);
        }
        return mono;
    }

    srt::Expected<double> chooseKnob(const std::optional<double> &given,
                                     const Api::Common::L1::Knob &knob, double fallback,
                                     std::string_view what) {
        if (!knob.honored) {
            return fallback;
        }
        if (!given) {
            return knob.defaultValue;
        }
        if (*given < knob.minimum || *given > knob.maximum) {
            return srt::Error(srt::Error::InvalidArgument, std::string(what) + " must be between " +
                                                               std::to_string(knob.minimum) +
                                                               " and " +
                                                               std::to_string(knob.maximum));
        }
        return *given;
    }

    srt::Expected<int> chooseKnob(const std::optional<int> &given,
                                  const Api::Common::L1::IntKnob &knob, int fallback,
                                  std::string_view what) {
        if (!knob.honored) {
            return fallback;
        }
        if (!given) {
            return knob.defaultValue;
        }
        if (*given < knob.minimum || *given > knob.maximum) {
            return srt::Error(srt::Error::InvalidArgument, std::string(what) + " must be between " +
                                                               std::to_string(knob.minimum) +
                                                               " and " +
                                                               std::to_string(knob.maximum));
        }
        return *given;
    }

    bool chooseKnob(const std::optional<bool> &given, const Api::Common::L1::FlagKnob &knob,
                    bool fallback) {
        if (!knob.honored) {
            return fallback;
        }
        return given.value_or(knob.defaultValue);
    }

}
