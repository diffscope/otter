#include <otter/Support/KnownNotes.h>

namespace otter::support {

    srt::Expected<void> checkKnownNotes(const std::vector<Api::Note::L1::KnownNote> &notes,
                                        const Api::Common::L1::AudioSegment &audio) {
        const double spanEnd = audio.startTime + audio.duration();
        double previousEnd = audio.startTime;
        for (const auto &note : notes) {
            if (note.duration <= 0) {
                return srt::Error(srt::Error::InvalidArgument, "a known note has no duration");
            }
            if (note.start < audio.startTime - TIME_EPSILON) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "a known note starts before the audio does");
            }
            if (note.start < previousEnd - TIME_EPSILON) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "the known notes overlap or are out of order");
            }
            previousEnd = note.start + note.duration;
            if (previousEnd > spanEnd + TIME_EPSILON) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "a known note ends after the audio does");
            }
        }
        return srt::Expected<void>();
    }

}
