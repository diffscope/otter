#ifndef OTTER_KNOWNNOTES_H
#define OTTER_KNOWNNOTES_H

#include <vector>

#include <synthrt/Support/Expected.h>

#include <otter/Api/Common/1/CommonApiL1.h>
#include <otter/Api/Note/1/NoteApiL1.h>

/// Checks shared by the Note providers, so that the stub and the shipped variant reject the same
/// known notes. This header is not part of the public API; see ManifestValues.h.
namespace otter::support {

    /// The tolerance, in seconds, within which two times on the host's timeline are equal.
    ///
    /// Times reach a provider as sums of doubles computed by the host, so a note intended to
    /// start at the start of the span, or at the end of the previous note, may differ from that
    /// time by a rounding error. The tolerance is far below the length of any frame, so it absorbs
    /// rounding errors and nothing else.
    inline constexpr double TIME_EPSILON = 1e-9;

    /// Checks that \a notes lie inside \a audio: each note has a positive duration, no note
    /// starts before the span or ends after it, and the notes are in ascending order without
    /// overlap.
    ///
    /// Known notes are stated on the host's timeline, like every time in the contract. A note
    /// outside the span is a caller error and is not clamped, because clamping would move the
    /// boundaries that the caller requested to keep.
    ///
    /// \return An empty value if every note satisfies these conditions, or an
    /// \c InvalidArgument error that describes the first violation.
    srt::Expected<void> checkKnownNotes(const std::vector<Api::Note::L1::KnownNote> &notes,
                                        const Api::Common::L1::AudioSegment &audio);

}

#endif // OTTER_KNOWNNOTES_H
