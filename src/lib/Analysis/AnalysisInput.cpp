#include <otter/Analysis/Provider/AnalysisInput.h>

#include <string>
#include <utility>

namespace otter {

    srt::Expected<PreparedSamples>
        PreparedSamples::prepare(const Api::Common::L1::AudioSegment &audio, int sampleRate,
                                 int channelCount, double maxSegmentDuration) {
        if (channelCount != 1) {
            // This code cannot widen a span, so a model that declares a channel count other than
            // one cannot receive the audio it declares. Rejecting the model at the first execution
            // is preferable to supplying silently averaged audio.
            return srt::Error(srt::Error::FeatureNotSupported,
                              "this build supports only models with one channel, and the model "
                              "declares " +
                                  std::to_string(channelCount) + " channels");
        }
        if (audio.sampleRate != sampleRate) {
            return srt::Error(srt::Error::InvalidArgument,
                              "this model requires " + std::to_string(sampleRate) +
                                  " Hz, and the audio is at " + std::to_string(audio.sampleRate) +
                                  " Hz; the host must resample the audio");
        }
        if (audio.channelCount < 1) {
            return srt::Error(srt::Error::InvalidArgument, "the audio declares no channels");
        }
        if (audio.samples.empty()) {
            return srt::Error(srt::Error::InvalidArgument, "the audio holds no samples");
        }
        if (audio.samples.size() % static_cast<std::size_t>(audio.channelCount) != 0) {
            // The division below would drop a truncated last frame. Dropping it shortens the span
            // by a fraction of a frame without any other visible effect, and the discrepancy
            // appears much later as drift.
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
            return PreparedSamples::view(audio.samples);
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
        return PreparedSamples::own(std::move(mono));
    }

}
