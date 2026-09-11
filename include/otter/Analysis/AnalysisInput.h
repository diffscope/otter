#ifndef OTTER_ANALYSISINPUT_H
#define OTTER_ANALYSISINPUT_H

#include <optional>
#include <string_view>
#include <vector>

#include <synthrt/Support/Expected.h>

#include <otter/Api/Common/1/CommonApiL1.h>
#include <otter/otter_global.h>

namespace otter {

    /// Checks one span against what a model accepts, and hands back the samples it will be fed.
    ///
    /// The two adaptations are deliberately not alike. Channels are averaged down to one, because
    /// that is arithmetic the caller would otherwise be asked to do for no reason and the result
    /// is the same either way. The sample rate is not touched, because resampling changes the
    /// answer, and doing it silently would hide that the host prepared the wrong audio.
    ///
    /// \a sampleRate and \a channelCount are what the model declares.
    OTTER_EXPORT srt::Expected<std::vector<float>>
        prepareSamples(const Api::Common::L1::AudioSegment &audio, int sampleRate,
                       int channelCount, double maxSegmentDuration);

    /// Returns the value to use for a knob, or why the one supplied cannot be used.
    ///
    /// An empty \a given selects \a fallback, which is the module's own default. A value outside
    /// the range the module declares is refused rather than clamped: a caller that asked for a
    /// setting the module cannot honor should learn that, not receive a different answer under the
    /// name of the one it asked for.
    /// \{
    OTTER_EXPORT srt::Expected<double> chooseKnob(const std::optional<double> &given,
                                                  double minimum, double maximum, double fallback,
                                                  std::string_view what);

    OTTER_EXPORT srt::Expected<int> chooseKnob(const std::optional<int> &given, int minimum,
                                               int maximum, int fallback, std::string_view what);
    /// \}

}

#endif // OTTER_ANALYSISINPUT_H
