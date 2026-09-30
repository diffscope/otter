#include "NonSpeech.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <utility>

namespace otter::hfa {

    namespace {

        /// Converts the class logits of one frame into probabilities.
        ///
        /// The computation stays in \c float throughout, as in the reference: every returned
        /// probability is compared against a threshold and never multiplied into a time, so an
        /// earlier widening would gain no accuracy and make the comparison diverge from the
        /// reference.
        std::vector<float> softmax(const std::vector<float> &logits) {
            const auto largest = *std::max_element(logits.begin(), logits.end());
            std::vector<float> probabilities(logits.size());
            float sum = 0;

            for (std::size_t i = 0; i < logits.size(); ++i) {
                probabilities[i] = std::exp(logits[i] - largest);
                sum += probabilities[i];
            }

            for (auto &probability : probabilities) {
                probability /= sum;
            }
            return probabilities;
        }

    }

    NonSpeech::NonSpeech(std::vector<std::string> classNames, int hopSize, int sampleRate)
        : m_classNames(std::move(classNames)), m_hop(hopSize), m_rate(sampleRate) {
        // The reference divides these two values as floats and keeps the quotient in float. This
        // member keeps the quotient in double without loss of accuracy, because the float quotient
        // differs from the exact ratio: 441/44100 rounded to float is above the exact 0.01, which
        // would round a knob's frame count up. This member is used as a divisor, not a factor, so
        // it is kept exact; the product in span() reproduces the reference's single precision.
        m_frameInterval = static_cast<double>(hopSize) / sampleRate;
    }

    srt::Expected<std::vector<Word>>
        NonSpeech::decode(std::vector<std::vector<float>> logits, double wavLength,
                          const std::vector<std::string> &labels) const {
        std::vector<Word> result;
        if (labels.empty()) {
            return result;
        }
        if (logits.empty()) {
            return srt::Error(srt::Error::InvalidArgument,
                              "the non-speech decoder was given no classes");
        }

        // One curve per class. The model may produce more frames than the audio covers, as a pass
        // over a padded waveform does; the surplus frames are dropped below.
        std::vector<std::vector<float>> adjusted = std::move(logits);
        const auto availableFrames = static_cast<int>(adjusted.front().size());
        const auto numClasses = static_cast<int>(adjusted.size());
        auto numFrames = wavLength > 0
                             ? static_cast<int>((static_cast<float>(wavLength) * m_rate + 0.5f) /
                                                static_cast<float>(m_hop))
                             : availableFrames;

        for (const auto &frames : adjusted) {
            if (wavLength > 0 && static_cast<int>(frames.size()) < numFrames) {
                // The length measured by the caller disagrees with the model output. The spans
                // would be placed at times beyond the audio they describe, so the mismatch is
                // reported instead of being trimmed into a plausible result.
                return srt::Error(srt::Error::InvalidArgument,
                                  "the non-speech logits carry " +
                                      std::to_string(frames.size()) + " frames, fewer than the " +
                                      std::to_string(numFrames) +
                                      " frames computed from the audio length");
            }
        }
        for (auto &frames : adjusted) {
            if (static_cast<int>(frames.size()) > numFrames) {
                frames.resize(static_cast<std::size_t>(numFrames));
            }
        }

        if (numClasses <= 0 || numFrames <= 0) {
            return result;
        }

        // [class][frame], which is the layout that the run scan below requires. The values stay
        // in float throughout, as in the reference.
        std::vector<std::vector<float>> probabilities(static_cast<std::size_t>(numClasses),
                                                      std::vector<float>(
                                                          static_cast<std::size_t>(numFrames), 0.0f));
        std::vector<float> frameLogits(static_cast<std::size_t>(numClasses));
        for (int t = 0; t < numFrames; ++t) {
            for (int c = 0; c < numClasses; ++c) {
                frameLogits[static_cast<std::size_t>(c)] =
                    adjusted[static_cast<std::size_t>(c)][static_cast<std::size_t>(t)];
            }

            const auto frameProbabilities = softmax(frameLogits);
            for (int c = 0; c < numClasses; ++c) {
                probabilities[static_cast<std::size_t>(c)][static_cast<std::size_t>(t)] =
                    frameProbabilities[static_cast<std::size_t>(c)];
            }
        }

        for (const auto &label : labels) {
            const auto found = std::find(m_classNames.begin(), m_classNames.end(), label);
            if (found == m_classNames.end()) {
                // The caller requested a class that this model does not define. An empty result
                // for the class would be indistinguishable from "none found", so the label is
                // rejected instead.
                return srt::Error(srt::Error::InvalidArgument,
                                  "the non-speech class " + label +
                                      " is not declared by the model");
            }

            const auto index = static_cast<std::size_t>(std::distance(m_classNames.begin(), found));
            auto decoded = spans(probabilities[index], label);
            result.insert(result.end(), std::make_move_iterator(decoded.begin()),
                          std::make_move_iterator(decoded.end()));
        }

        return result;
    }

    void NonSpeech::setThreshold(double threshold) {
        m_threshold = static_cast<float>(threshold);
    }

    void NonSpeech::setMinimumDuration(double seconds) {
        // The reference converts frames to time as `end * frame_length`, so a span of \p seconds
        // covers the frames whose index is below this quotient, and the truncation discards the
        // fractional frame of a shorter span.
        m_minFrames = static_cast<int>(seconds / m_frameInterval);
    }

    std::vector<Word> NonSpeech::spans(const std::vector<float> &probabilities,
                                      const std::string &label) const {
        std::vector<Word> words;
        int start = -1;
        int gapCount = 0;

        for (int i = 0; i < static_cast<int>(probabilities.size()); ++i) {
            if (probabilities[static_cast<std::size_t>(i)] >= m_threshold) {
                if (start == -1) {
                    start = i;
                }
                gapCount = 0;
            } else if (start != -1) {
                // A run interrupted for no more than maxGap frames remains one span, and a
                // trailing interruption is excluded from the span's end instead of splitting the
                // span in two.
                if (gapCount < m_maxGap) {
                    ++gapCount;
                } else {
                    const int end = i - gapCount - 1;
                    if (end > start && end - start >= m_minFrames) {
                        words.push_back(span(start, end, label));
                    }
                    start = -1;
                    gapCount = 0;
                }
            }
        }

        // A run still open at the end of the audio reaches no frame below the threshold, so it is
        // closed at the last frame instead of by the scan above. The reference measures this run
        // by its frame count alone, and the asymmetry here reproduces that behavior.
        if (start != -1 && static_cast<int>(probabilities.size()) - start >= m_minFrames) {
            words.push_back(span(start, static_cast<int>(probabilities.size()) - 1, label));
        }

        return words;
    }

    Word NonSpeech::span(int startFrame, int endFrame, const std::string &label) const {
        // The run's ends are inclusive frame indices. The reference converts each index into a
        // time by multiplying it by the frame length and subtracting afterwards; it performs the
        // multiplication in single precision and the subtraction in double, because
        // `frame_length_` is a float while the stored times are seconds in double. The order
        // affects the last bit: a frame count multiplied once and a difference of two times agree
        // only to about a hundredth of a microsecond, and the end of a span must equal the
        // aligner's own boundary for its last frame exactly.
        //
        // Both products are therefore computed in float, as in the reference, and the final
        // subtraction is the only double operation.
        const float frameLength = static_cast<float>(m_frameInterval);
        const auto startTime = static_cast<float>(startFrame) * frameLength;
        const auto endTime = static_cast<float>(endFrame) * frameLength;

        Word found;
        found.text = label;
        found.start = static_cast<double>(startTime);
        found.duration = static_cast<double>(endTime) - static_cast<double>(startTime);
        found.phones.push_back(Phone{label, found.start, found.duration});
        return found;
    }

}
