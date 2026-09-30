#ifndef OTTER_HFA_NONSPEECH_H
#define OTTER_HFA_NONSPEECH_H

#include <string>
#include <vector>

#include <synthrt/Support/Expected.h>

#include "Aligner.h"  // otter::hfa::Word

namespace otter::hfa {

    /// Converts the non-speech classes that a model predicts into spans on the timeline.
    ///
    /// These classes are the breaths, lip noises and pauses that a singing-voice aligner labels
    /// separately from the sung phonemes.
    class NonSpeech {
    public:
        /// \param classNames the model's classes in index order, including the "None" class that
        ///        the provider prepends at index 0
        /// \param hopSize samples between adjacent frames
        /// \param sampleRate rate the frames were computed at
        NonSpeech(std::vector<std::string> classNames, int hopSize, int sampleRate);

        /// Decodes the spans of the requested classes.
        ///
        /// \param logits [classes][frames] of batch 0; taken by value because the decode trims it
        ///        in place, so a caller that no longer needs it moves it in
        /// \param labels the classes to report, in the order of the result
        /// \param wavLength length of the audio in seconds, used to clip the last frames
        /// \return The spans of every class in \p labels, or \c srt::Error::InvalidArgument if
        ///         \p logits is empty, has fewer frames than \p wavLength requires, or a label is
        ///         not a class of the model. An empty \p labels yields an empty result.
        srt::Expected<std::vector<Word>> decode(std::vector<std::vector<float>> logits,
                                                double wavLength,
                                                const std::vector<std::string> &labels) const;

        /// Sets the probability at or above which a frame belongs to the class (default 0.5).
        void setThreshold(double threshold);

        /// Sets the shortest reported span, in seconds (default 0.1).
        void setMinimumDuration(double seconds);

    private:
        /// Returns one span of a single class, from frame \p startFrame to frame \p endFrame, both
        /// inclusive.
        Word span(int startFrame, int endFrame, const std::string &label) const;

        /// Scans the probability curve of one class and divides it into the spans it covers.
        std::vector<Word> spans(const std::vector<float> &probabilities,
                                const std::string &label) const;

        std::vector<std::string> m_classNames;
        double m_frameInterval; // hop / rate, set by the constructor
        int m_hop;              // samples between adjacent frames
        int m_rate;             // rate the frames were computed at
        float m_threshold = 0.5f;
        int m_maxGap = 5;     // longest interruption, in frames, that keeps a run one span
        int m_minFrames = 10; // frame count of the shortest reported span
    };

}

#endif // OTTER_HFA_NONSPEECH_H
