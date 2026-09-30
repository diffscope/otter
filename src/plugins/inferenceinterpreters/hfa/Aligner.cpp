#include "Aligner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace otter::hfa {

    namespace {

        /// Log softmax over the class axis of a [classes][frames] matrix.
        ///
        /// The reference takes the axis as an argument but is called only with the class axis,
        /// which is the axis computed here.
        ///
        /// The reference also computes a softmax of the same matrix and keeps the result as its
        /// per-frame predictions. The decode never reads those probabilities, because the masked
        /// logits below determine the path; this port therefore omits that computation, and no
        /// boundary is affected.
        ///
        /// The 1e-12 inside the log is taken from the reference: without it, a column whose
        /// exponentials all underflowed would produce the log of zero and pass -inf to the Viterbi
        /// pass.
        std::vector<std::vector<float>> logSoftmax2d(const std::vector<std::vector<float>> &x) {
            if (x.empty()) {
                return {};
            }

            const std::size_t dim1 = x.size();
            const std::size_t dim2 = x[0].size();

            std::vector<std::vector<float>> result(dim1, std::vector<float>(dim2));

            for (std::size_t j = 0; j < dim2; ++j) {
                float maxVal = -std::numeric_limits<float>::infinity();
                for (std::size_t i = 0; i < dim1; ++i) {
                    if (j < x[i].size()) {
                        maxVal = std::max(maxVal, x[i][j]);
                    }
                }

                float logSumExp = 0.0f;
                for (std::size_t i = 0; i < dim1; ++i) {
                    if (j < x[i].size()) {
                        logSumExp += std::exp(x[i][j] - maxVal);
                    }
                }
                logSumExp = maxVal + std::log(logSumExp + 1e-12f);

                for (std::size_t i = 0; i < dim1; ++i) {
                    if (j < x[i].size()) {
                        result[i][j] = x[i][j] - logSumExp;
                    } else {
                        result[i][j] = -std::numeric_limits<float>::infinity();
                    }
                }
            }
            return result;
        }

        float sigmoid(float x) {
            return 1.0f / (1.0f + std::exp(-x));
        }

        /// Runs the forward pass of the three-state Viterbi.
        ///
        /// Each transition includes an edge cost derived from the running maximum of the source
        /// phoneme's log-probabilities. The reference adds `curr_ph_max_prob_log[s] * (T / S)` to
        /// every transition, which prevents a long phoneme from receiving too few frames if the
        /// audio is longer than the sequence length implies.
        ///
        /// \a scoreScale is `static_cast<float>(T) / S`, computed once per pass. The reference
        /// recomputes that division inside the loops with identical operands, so the value is the
        /// same; this hoisting is the only deviation of this port from the reference's sequence of
        /// operations.
        void forwardPass(const int T, const int S, const std::vector<std::vector<float>> &probLog,
                         const std::vector<float> &edgeProb, std::vector<float> &maxProbLog,
                         std::vector<std::vector<float>> &dp, const std::vector<int> &phonemeIds,
                         const int separatorClass, const int skipPadding,
                         std::vector<std::vector<int>> &backtrackS) {
            std::vector<float> edgeProbLog(T);
            std::vector<float> notEdgeProbLog(T);
            for (int t = 0; t < T; ++t) {
                edgeProbLog[t] = std::log(edgeProb[t] + 1e-6f);
                notEdgeProbLog[t] = std::log(1.0f - edgeProb[t] + 1e-6f);
            }

            std::vector<bool> maskReset(S);
            for (int s = 0; s < S; ++s) {
                // Every SP in the sequence starts its span with a reset running maximum instead of
                // inheriting the running maximum of the preceding phonemes.
                maskReset[s] = phonemeIds[s] == separatorClass;
            }

            const float scoreScale = static_cast<float>(T) / S;

            std::vector<float> prob1(S);
            std::vector<float> prob2(S, -std::numeric_limits<float>::infinity());
            std::vector<float> prob3(S, -std::numeric_limits<float>::infinity());

            for (int t = 1; t < T; ++t) {
                std::vector<float> candidateVals(S - skipPadding,
                                                 -std::numeric_limits<float>::infinity());
                for (int srcIdx = 0; srcIdx < S - skipPadding; ++srcIdx) {
                    candidateVals[srcIdx] = dp[srcIdx][t - 1] + probLog[srcIdx][t] + edgeProbLog[t] +
                                            maxProbLog[srcIdx] * scoreScale;
                }

                for (int s = 0; s < S; ++s) {
                    // Type 1: stay on the current phoneme, paying the "no edge here" cost.
                    prob1[s] = dp[s][t - 1] + probLog[s][t] + notEdgeProbLog[t];

                    // Type 2: step to the next phoneme, crossing a boundary.
                    if (s > 0) {
                        prob2[s] = dp[s - 1][t - 1] + probLog[s - 1][t] + edgeProbLog[t] +
                                   maxProbLog[s - 1] * scoreScale;
                    }

                    // Type 3: skip a short run of phonemes. Only a jump that lands on SP (or on the
                    // last phoneme) is allowed; this transition represents phonemes that were not
                    // sung.
                    if (s >= skipPadding) {
                        int idxArr = s - skipPadding + 1;
                        if (idxArr < 0) {
                            idxArr = 0;
                        }
                        if (idxArr > S - 1) {
                            idxArr = S - 1;
                        }

                        if (idxArr >= S - 1 || phonemeIds[idxArr] == separatorClass) {
                            prob3[s] = candidateVals[s - skipPadding];
                        }
                    }

                    float maxVal = prob1[s];
                    int bestType = 0;

                    if (prob2[s] > maxVal) {
                        maxVal = prob2[s];
                        bestType = 1;
                    }
                    if (prob3[s] > maxVal) {
                        maxVal = prob3[s];
                        bestType = 2;
                    }

                    dp[s][t] = maxVal;
                    backtrackS[s][t] = bestType;

                    if (bestType == 0) {
                        if (probLog[s][t] > maxProbLog[s]) {
                            maxProbLog[s] = probLog[s][t];
                        }
                    } else {
                        maxProbLog[s] = probLog[s][t];
                    }

                    if (maskReset[s]) {
                        maxProbLog[s] = 0.0f;
                    }
                }

                // Types 2 and 3 depend only on the previous frame, so a stale value would be read
                // again by the next iteration. The reference clears both here; prob1 is instead
                // overwritten unconditionally at the top of the loop.
                std::fill(prob2.begin(), prob2.end(), -std::numeric_limits<float>::infinity());
                std::fill(prob3.begin(), prob3.end(), -std::numeric_limits<float>::infinity());
            }
        }

        /// Runs the three-state Viterbi and walks the backtracking table back into boundaries.
        ///
        /// \param probLog [vocabulary][frames] log-probabilities, already masked.
        /// \param edgeProb per-frame boundary probability, already the sum of adjacent edges.
        /// \param[out] phoneIndices one entry per decoded phone, indexing into \a phonemeIds.
        /// \param[out] phoneFrames  index of the frame each decoded phone started on.
        /// \param[out] frameConfidence per-frame confidence along the chosen path.
        /// \returns An empty value, or \c srt::Error::InvalidArgument if the logits cover fewer
        ///          frames than \a T.
        srt::Expected<void> decodeFrames(const std::vector<int> &phonemeIds,
                                         const int separatorClass,
                                         const std::vector<std::vector<float>> &probLog,
                                         const std::vector<float> &edgeProb,
                                         std::vector<int> &phoneIndices,
                                         std::vector<int> &phoneFrames,
                                         std::vector<float> &frameConfidence, const int T) {
            const int S = static_cast<int>(phonemeIds.size());
            if (S == 0 || T == 0) {
                return srt::Expected<void>();
            }

            // The reference returns early here and leaves its members unchanged, which the caller
            // cannot distinguish from a successful empty decode. This port rejects the input
            // instead, because the condition indicates that the caller's frame count and the
            // logits disagree.
            if (probLog.empty() || probLog[0].size() < static_cast<std::size_t>(T)) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "the phoneme logits hold " +
                                      std::to_string(probLog.empty() ? 0 : probLog[0].size()) +
                                      " frames, fewer than the " + std::to_string(T) +
                                      " frames that the audio length requires");
            }

            // [S][T]: one row per phoneme in the sequence, holding its log-probability per frame.
            // A phoneme whose class index the logits do not carry keeps -inf and is never chosen.
            std::vector<std::vector<float>> selectedLog(
                S, std::vector<float>(T, -std::numeric_limits<float>::infinity()));
            for (int s = 0; s < S; ++s) {
                const int vocabIdx = phonemeIds[s];
                if (vocabIdx >= 0 && static_cast<std::size_t>(vocabIdx) < probLog.size()) {
                    for (int t = 0; t < T; ++t) {
                        if (t < static_cast<int>(probLog[vocabIdx].size())) {
                            selectedLog[s][t] = probLog[vocabIdx][t];
                        }
                    }
                }
            }

            std::vector<std::vector<float>> dp(S, std::vector<float>(T,
                                                                     -std::numeric_limits<
                                                                         float>::infinity()));
            std::vector<float> maxProbLog(S, -std::numeric_limits<float>::infinity());
            std::vector<std::vector<int>> backtrackS(S, std::vector<int>(T, -1));

            dp[0][0] = selectedLog[0][0];
            maxProbLog[0] = selectedLog[0][0];

            // A leading SP is skippable, so the second phoneme may own frame 0 as well.
            if (S > 1 && phonemeIds[0] == separatorClass) {
                dp[1][0] = selectedLog[1][0];
                maxProbLog[1] = selectedLog[1][0];
            }

            // The type-3 jump reads the candidate two positions back, so the padding is two
            // phonemes if the sequence has at least two. A one-phoneme sequence cannot jump, and
            // the loop inside forwardPass never runs for it, so the smaller padding is never read.
            const int skipPadding = S >= 2 ? 2 : 1;
            forwardPass(T, S, selectedLog, edgeProb, maxProbLog, dp, phonemeIds, separatorClass,
                        skipPadding, backtrackS);

            // The reference allows the alignment to end on the phoneme before a trailing SP
            // instead of on the SP itself, and selects the higher-scoring of the two.
            int finalS = S - 1;
            if (S >= 2 && dp[S - 2][T - 1] > dp[S - 1][T - 1] &&
                phonemeIds[S - 1] == separatorClass) {
                finalS = S - 2;
            }

            std::vector<int> tmpPhoneIndices;
            std::vector<int> tmpPhoneFrames;
            std::vector<float> pathScore(T, 0.0f);

            int s = finalS;
            for (int t = T - 1; t >= 0; --t) {
                if (s < 0 || s >= S) {
                    break;
                }

                pathScore[t] = dp[s][t];

                // A zero entry indicates that the path stayed on the current phoneme at this
                // frame, so nothing is recorded for the frame and the walk remains on that phoneme.
                if (backtrackS[s][t] != 0) {
                    tmpPhoneIndices.push_back(s);
                    tmpPhoneFrames.push_back(t);

                    if (backtrackS[s][t] == 1) {
                        s -= 1;
                    } else if (backtrackS[s][t] == 2) {
                        s -= 2;
                    }
                }
            }

            std::reverse(tmpPhoneIndices.begin(), tmpPhoneIndices.end());
            std::reverse(tmpPhoneFrames.begin(), tmpPhoneFrames.end());

            phoneIndices = std::move(tmpPhoneIndices);
            phoneFrames = std::move(tmpPhoneFrames);

            // The confidence is the ratio between the path score at this frame and the path score
            // at the previous frame, that is, a likelihood ratio rather than a probability. The
            // leading zero is the reference's padding: the first frame is scored against 0.
            //
            // No code reads the confidence yet. It is kept, with pathScore and paddedScore, so that
            // this walk remains a line-for-line transliteration of the reference, on which the
            // numeric comparison depends, and so that a per-word confidence, which is an open
            // question of the contract, can be derived here without a new port.
            frameConfidence.resize(T);
            std::vector<float> paddedScore(T + 1, 0.0f);
            for (int t = 0; t < T; ++t) {
                paddedScore[t + 1] = pathScore[t];
            }
            for (int t = 0; t < T; ++t) {
                frameConfidence[t] = std::exp(paddedScore[t + 1] - paddedScore[t]);
            }

            return srt::Expected<void>();
        }

        /// The span of one phoneme in seconds.
        struct Span {
            float start;
            float end;
        };

        /// Returns the end of a word's last phone, or the word's start if the word has no phone.
        ///
        /// Both values are zero for a default-constructed word, so the callers here treat a word
        /// without phones like a zero-length span. This is deliberate: the reference's
        /// `phones.back()` on an empty list was undefined behavior, and every operation that can
        /// reach this case rejects the word under either interpretation.
        float lastPhoneEnd(const Word &word) {
            if (word.phones.empty()) {
                return static_cast<float>(word.start);
            }
            const Phone &last = word.phones.back();
            return static_cast<float>(last.start + last.duration);
        }

        float firstPhoneEnd(const Word &word) {
            if (word.phones.empty()) {
                return static_cast<float>(word.start);
            }
            const Phone &first = word.phones.front();
            return static_cast<float>(first.start + first.duration);
        }

        float lastPhoneStart(const Word &word) {
            if (word.phones.empty()) {
                return static_cast<float>(word.start);
            }
            return static_cast<float>(word.phones.back().start);
        }

        /// Moves a word's start together with the start of its first phone.
        ///
        /// The span is the reference's `[start, end)`, so raising the start shortens the duration
        /// by the same amount. Adding the delta to the start and subtracting it from the duration
        /// do not always round to the identical double; this is inherent to the representation
        /// and matches the reference.
        ///
        /// \return \c true if the start was moved; \c false if the word has no phone or the new
        ///         start would reach the end of the first phone and invert it.
        bool moveStart(Word &word, float newStart) {
            newStart = std::max(0.0f, newStart);
            if (!(newStart >= 0.0f && newStart < firstPhoneEnd(word))) {
                return false;
            }

            const double delta = static_cast<double>(newStart) - word.start;
            word.start = static_cast<double>(newStart);
            word.duration -= delta;

            Phone &first = word.phones.front();
            first.start = static_cast<double>(newStart);
            first.duration -= delta;
            return true;
        }

        /// Moves a word's end together with the end of its last phone.
        ///
        /// \return \c true if the end was moved; \c false if the word has no phone or the new end
        ///         would not lie after the last phone's start.
        bool moveEnd(Word &word, float newEnd) {
            newEnd = std::max(0.0f, newEnd);
            if (!(newEnd > lastPhoneStart(word) && newEnd >= 0.0f)) {
                return false;
            }

            word.duration = static_cast<double>(newEnd) - word.start;

            Phone &last = word.phones.back();
            last.duration = static_cast<double>(newEnd) - last.start;
            return true;
        }

        /// Returns whether \a a and \a b overlap, tested exactly as the reference's
        /// `overlapping_words` tests it.
        bool overlaps(const Word &a, const Word &b) {
            return !(a.start + a.duration <= b.start || a.start >= b.start + b.duration);
        }

        /// Removes \a removeInterval from \a rawInterval.
        ///
        /// \return The remaining parts of \a rawInterval, which are empty if \a removeInterval
        ///         covers it entirely; \c srt::Error::InvalidArgument if either interval is empty
        ///         or inverted.
        srt::Expected<std::vector<std::pair<float, float>>>
        removeOverlappingIntervals(const std::pair<float, float> &rawInterval,
                                   const std::pair<float, float> &removeInterval) {
            const float rStart = rawInterval.first;
            const float rEnd = rawInterval.second;
            const float mStart = removeInterval.first;
            const float mEnd = removeInterval.second;

            if (!(rStart < rEnd)) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "raw_interval.start must be smaller than raw_interval.end");
            }
            if (!(mStart < mEnd)) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "remove_interval.start must be smaller than remove_interval.end");
            }

            const float overlapStart = std::max(rStart, mStart);
            const float overlapEnd = std::min(rEnd, mEnd);

            if (overlapStart >= overlapEnd) {
                return std::vector<std::pair<float, float>>{rawInterval};
            }

            std::vector<std::pair<float, float>> result;
            if (rStart < overlapStart) {
                result.emplace_back(rStart, overlapStart);
            }
            if (overlapEnd < rEnd) {
                result.emplace_back(overlapEnd, rEnd);
            }
            return result;
        }

    }

    Aligner::Aligner(std::map<std::string, int> vocabulary, std::string separator,
                     const int hopSize, const int sampleRate)
        : m_vocabulary(std::move(vocabulary)), m_separator(std::move(separator)),
          m_hopSize(hopSize), m_sampleRate(sampleRate) {
        // The provider verifies at load time that the separator is in the vocabulary.
        const auto it = m_vocabulary.find(m_separator);
        m_separatorClass = it == m_vocabulary.end() ? 0 : it->second;
        m_frameLength = static_cast<float>(m_hopSize) / m_sampleRate;
    }

    int Aligner::frameCount(const double wavLength) const {
        // The reference rounds with the same `+ 0.5f` in float before truncating, which aligns
        // its frame indices with the model's own front end.
        return static_cast<int>((static_cast<float>(wavLength) * m_sampleRate + 0.5f) / m_hopSize);
    }

    srt::Expected<std::vector<Word>> Aligner::decode(std::vector<std::vector<float>> phFrameLogits,
                                                     std::vector<float> phEdgeLogits,
                                                     const Phrase &phrase,
                                                     const double wavLength) const {
        // ---- phoneme labels to class indices ------------------------------------------------
        std::vector<int> phonemeIds;
        phonemeIds.reserve(phrase.phonemes.size());
        for (const auto &phoneme : phrase.phonemes) {
            const auto it = m_vocabulary.find(phoneme);
            if (it == m_vocabulary.end()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "the phoneme '" + phoneme + "' is not in the vocabulary");
            }
            phonemeIds.push_back(it->second);
        }

        const std::size_t vocabSize = m_vocabulary.size();

        // Every class that the line does not use is lowered far below the used classes, so that
        // a spurious phoneme predicted by the model can never win a frame. The separator's class
        // remains selectable in all cases, because the splitter inserts the separator itself.
        std::vector<float> phMask(vocabSize, 1e9f);
        for (const int id : phonemeIds) {
            if (id >= 0 && static_cast<std::size_t>(id) < vocabSize) {
                phMask[id] = 0.0f;
            }
        }
        if (m_separatorClass >= 0 && static_cast<std::size_t>(m_separatorClass) < vocabSize) {
            phMask[m_separatorClass] = 0.0f;
        }

        // The frame count is derived from the audio length, not from the logits: the model runs on
        // a padded window, so the logits are normally longer and are truncated here.
        const int numFrames = frameCount(wavLength);
        if (numFrames < 1) {
            // The reference decodes nothing here and returns an empty list, which the caller
            // cannot distinguish from audio in which none of the words was found. No word fits in
            // less than a frame, so the audio is rejected instead.
            return srt::Error(srt::Error::InvalidArgument,
                              "the audio is shorter than one frame of the model");
        }

        if (phFrameLogits.empty() || phFrameLogits[0].empty()) {
            return srt::Error(srt::Error::InvalidArgument, "ph_frame_logits is empty");
        }

        // The reference also runs a softmax here and keeps the result as its per-frame
        // predictions. The decode never reads those probabilities, because the mask and the
        // log-softmax below determine the path; therefore only the log-softmax is computed.
        //
        // The reference takes the whole batch and extracts batch 0 here; this function receives
        // batch 0 by value, so the mask below modifies it in place.
        std::vector<std::vector<float>> frameLogits = std::move(phFrameLogits);
        for (std::size_t v = 0; v < frameLogits.size(); ++v) {
            if (frameLogits[v].size() > static_cast<std::size_t>(numFrames)) {
                frameLogits[v].resize(numFrames);
            }
        }

        for (std::size_t v = 0; v < frameLogits.size(); ++v) {
            // The reference indexes the mask without a bounds check. Classes outside the
            // vocabulary have no mask entry, and skipping them affects no boundary, so they are
            // skipped instead of being read past the end of the mask.
            if (v >= phMask.size()) {
                continue;
            }
            for (int t = 0; t < numFrames; ++t) {
                if (t < static_cast<int>(frameLogits[v].size())) {
                    frameLogits[v][t] -= phMask[v];
                }
            }
        }

        const auto probLog = logSoftmax2d(frameLogits);

        if (phEdgeLogits.empty()) {
            return srt::Error(srt::Error::InvalidArgument, "ph_edge_logits is empty");
        }

        std::vector<float> edgeLogits = std::move(phEdgeLogits);
        if (edgeLogits.size() > static_cast<std::size_t>(numFrames)) {
            edgeLogits.resize(numFrames);
        }

        std::vector<float> edgePred;
        edgePred.reserve(edgeLogits.size());
        for (const float logit : edgeLogits) {
            const float val = sigmoid(logit);
            edgePred.push_back(std::max(0.0f, std::min(1.0f, val)));
        }

        // The subsequent correction interprets an edge as a rate of change, so the curve is
        // differentiated first: a rise at frame i places the boundary between i and i+1.
        std::vector<float> edgeDiff(edgePred.size(), 0.0f);
        if (edgePred.size() > 1) {
            for (std::size_t i = 0; i + 1 < edgePred.size(); ++i) {
                edgeDiff[i] = edgePred[i + 1] - edgePred[i];
            }
        }

        // The boundary probability used by the Viterbi pass. Summing the edge of a frame with the
        // edge of the previous frame counts a boundary that spans two frames once, because the
        // model marks a transition across the pair. The first frame has no predecessor and keeps
        // its own value.
        std::vector<float> edgeProb(edgePred.size());
        edgeProb[0] = edgePred[0];
        for (std::size_t i = 1; i < edgePred.size(); ++i) {
            edgeProb[i] = edgePred[i] + edgePred[i - 1];
            if (edgeProb[i] < 0.0f) {
                edgeProb[i] = 0.0f;
            }
            if (edgeProb[i] > 1.0f) {
                edgeProb[i] = 1.0f;
            }
        }

        std::vector<int> phoneIndices;
        std::vector<int> phoneFrames;
        std::vector<float> frameConfidence;
        auto ordered = decodeFrames(phonemeIds, m_separatorClass, probLog, edgeProb, phoneIndices,
                                    phoneFrames, frameConfidence, numFrames);
        if (!ordered) {
            return ordered.takeError();
        }

        // ---- frame indices to seconds -------------------------------------------------------
        // The Viterbi pass yields whole frames, which would quantize every boundary to the frame
        // grid. The edge curve indicates the position of the transition inside the frame, so half
        // of its slope at that frame is added to the boundary, clipped to half a frame in either
        // direction. A larger slope does not describe a single transition, and a larger offset
        // would be meaningless.
        std::vector<float> phoneTime;
        phoneTime.reserve(phoneFrames.size());
        for (const int frame : phoneFrames) {
            float fraction = 0.0f;
            if (frame >= 0 && frame < static_cast<int>(edgeDiff.size())) {
                fraction = edgeDiff[frame] / 2.0f;
                if (fraction < -0.5f) {
                    fraction = -0.5f;
                }
                if (fraction > 0.5f) {
                    fraction = 0.5f;
                }
            }
            const float time = m_frameLength * (static_cast<float>(frame) + fraction);
            phoneTime.push_back(std::max(0.0f, time));
        }
        // The last boundary is the end of the audio, not a corrected frame, because no transition
        // follows it for the edge curve to describe.
        phoneTime.push_back(m_frameLength * static_cast<float>(numFrames));

        // The reference builds these intervals only to index them one-to-one with the decoded
        // phonemes, and a decoded sequence longer than the frame count would read past the end.
        // A consistent model cannot produce such a sequence, but the check is inexpensive, and a
        // silent out-of-range read would place a phone at an undefined time.
        const std::size_t phoneCount = phoneIndices.size();
        if (phoneCount + 1 > phoneTime.size()) {
            return srt::Error(srt::Error::InvalidArgument,
                              "the alignment decoded " + std::to_string(phoneCount) +
                                  " phonemes from " + std::to_string(numFrames) +
                                  " frames, which yields no span for the last of them");
        }

        std::vector<Span> phoneSpans(phoneCount);
        for (std::size_t i = 0; i < phoneCount; ++i) {
            phoneSpans[i] = Span{phoneTime[i], phoneTime[i + 1]};
        }

        // ---- phonemes to words --------------------------------------------------------------
        // The word grouping follows the phrase: consecutive decoded phonemes with the same word
        // index form one word. A phoneme whose index lies outside the phrase is dropped, which
        // leaves its word shorter than the lyrics imply.
        std::vector<Word> words;
        int lastWordIndex = -1;
        bool hasCurrentWord = false;
        Word currentWord;

        for (std::size_t i = 0; i < phoneCount; ++i) {
            const int phoneIndex = phoneIndices[i];
            if (phoneIndex < 0 || phoneIndex >= static_cast<int>(phrase.phonemes.size())) {
                continue;
            }

            const std::string &phoneText = phrase.phonemes[phoneIndex];

            if (phoneText == m_separator) {
                continue;
            }

            const Span &span = phoneSpans[i];

            int wordIndex;
            if (phrase.phonemeToWord.empty()) {
                wordIndex = phoneIndex;
            } else if (phoneIndex < static_cast<int>(phrase.phonemeToWord.size())) {
                wordIndex = phrase.phonemeToWord[phoneIndex];
            } else {
                wordIndex = phoneIndex;
            }

            std::string wordText;
            if (phrase.words.empty()) {
                wordText = phoneText;
            } else if (wordIndex < static_cast<int>(phrase.words.size())) {
                wordText = phrase.words[wordIndex];
            } else {
                wordText = phoneText;
            }

            if (wordIndex == lastWordIndex && hasCurrentWord) {
                // The rule is the reference's `append_phone`, including its tolerance: a phone that
                // does not start within 1e-6 of the end of the previous phone is dropped, and the
                // word keeps its current end. The tolerance is deliberately loose because it
                // absorbs the rounding error of boundary differences, not a genuine gap; for this
                // reason the test is not an equality test.
                if (std::abs(span.start - lastPhoneEnd(currentWord)) >= 1e-6f) {
                    continue;
                }
                currentWord.phones.push_back(
                    Phone{phoneText, static_cast<double>(span.start),
                          static_cast<double>(span.end - span.start)});
                currentWord.duration = static_cast<double>(span.end) - currentWord.start;
            } else {
                if (hasCurrentWord) {
                    words.push_back(std::move(currentWord));
                }
                currentWord = Word{};
                currentWord.text = wordText;
                currentWord.start = static_cast<double>(span.start);
                currentWord.duration = static_cast<double>(span.end - span.start);
                currentWord.phones.push_back(Phone{phoneText, static_cast<double>(span.start),
                                                   static_cast<double>(span.end - span.start)});
                hasCurrentWord = true;
                lastWordIndex = wordIndex;
            }
        }

        if (hasCurrentWord) {
            words.push_back(std::move(currentWord));
        }

        // The reference finally reduces `frame_confidence` to a single total confidence and keeps
        // both as members. No downstream code reads either value, because the word list is the
        // complete result; the values are therefore computed and discarded here.
        // `frameConfidence` remains in the signature only because it is an output of the Viterbi
        // walk.
        (void) frameConfidence;

        return words;
    }

    srt::Expected<void> Aligner::insertNonSpeech(std::vector<Word> &words, const Word &entry,
                                                 const double minimumDuration) {
        // A word without phones has no span to splice, and the reference rejects such a word
        // before the interval arithmetic instead of carrying a zero-length span.
        if (entry.phones.empty()) {
            return srt::Error(srt::Error::InvalidArgument,
                              "the non-speech entry " + entry.text + " has no phones");
        }

        if (words.empty()) {
            words.push_back(entry);
            return srt::Expected<void>();
        }

        bool anyOverlap = false;
        for (const Word &word : words) {
            if (overlaps(entry, word)) {
                anyOverlap = true;
                break;
            }
        }
        if (!anyOverlap) {
            words.push_back(entry);
            std::sort(words.begin(), words.end(),
                      [](const Word &a, const Word &b) { return a.start < b.start; });
            return srt::Expected<void>();
        }

        // The entry is trimmed against every word in the list, not only the overlapping words:
        // iterating over the whole list makes the result independent of the word order, and a
        // non-intersecting interval is returned unchanged.
        std::vector<std::pair<float, float>> pieces{
            {static_cast<float>(entry.start),
             static_cast<float>(entry.start + entry.duration)}};

        std::vector<std::pair<float, float>> trimmed;
        for (const Word &word : words) {
            trimmed.clear();
            for (const std::pair<float, float> &piece : pieces) {
                const auto remaining = removeOverlappingIntervals(
                    piece, {static_cast<float>(word.start),
                            static_cast<float>(word.start + word.duration)});
                if (!remaining) {
                    // This branch is reachable only if a word's span is empty, which the reference
                    // also skips with a log message instead of changing the intervals. The piece
                    // is kept instead of failing the whole insertion.
                    continue;
                }
                trimmed.insert(trimmed.end(), remaining->begin(), remaining->end());
            }
            pieces = trimmed;
        }

        // The remaining pieces are the parts of the entry that no word covers. A very short piece
        // is as likely to be an artifact of the frame grid as a real breath, so it is dropped.
        for (const std::pair<float, float> &piece : pieces) {
            if (static_cast<double>(piece.second - piece.first) < minimumDuration) {
                continue;
            }

            Word fragment{};
            fragment.text = entry.text;
            fragment.start = static_cast<double>(piece.first);
            fragment.duration = static_cast<double>(piece.second - piece.first);
            fragment.phones.push_back(Phone{entry.text, fragment.start, fragment.duration});
            words.push_back(std::move(fragment));
        }

        std::sort(words.begin(), words.end(),
                  [](const Word &a, const Word &b) { return a.start < b.start; });
        return srt::Expected<void>();
    }

    void Aligner::fillSmallGaps(std::vector<Word> &words, const double wavLength,
                                const double gapFill) {
        if (words.empty()) {
            return;
        }

        // The reference begins with an unconditional `words_[0].start = 0`, before any phone is
        // consulted: a list that starts before the audio is moved to start at zero, and the
        // duration is left unchanged so that the word's end does not move. The decoder never
        // produces such a start, because it clamps every boundary to zero when converting frames
        // to time; this branch therefore applies only to a list assembled by a caller. The first
        // phone is moved with the word, because a word starting after its first phone would not
        // contain that phone.
        if (words.front().start < 0) {
            words.front().start = 0;
            if (!words.front().phones.empty() && words.front().phones.front().start < 0) {
                words.front().phones.front().start = 0;
            }
        }

        // A first word preceded by a silence shorter than the gap fill is moved back to start at
        // zero. The second condition is the reference's guard against applying this to a word
        // of zero length, which has no phones to move.
        if (words.front().start > 0 && words.front().start < gapFill &&
            gapFill < words.front().start + words.front().duration) {
            (void) moveStart(words.front(), 0.0f);
        }

        // The tail is handled separately: if the last word ends within the gap fill of the end of
        // the audio, the remaining gap is absorbed instead of being left unlabelled.
        if (words.back().start + words.back().duration >= wavLength - gapFill) {
            (void) moveEnd(words.back(), static_cast<float>(wavLength));
        }

        for (std::size_t i = 1; i < words.size(); ++i) {
            const double gap = words[i].start - (words[i - 1].start + words[i - 1].duration);
            if (gap > 0 && gap <= gapFill) {
                (void) moveEnd(words[i - 1], static_cast<float>(words[i].start));
            }
        }
    }

    srt::Expected<void> Aligner::addSilence(std::vector<Word> &words, const double wavLength,
                                            const std::string &label) {
        // An empty list is left unchanged, as in the reference, instead of being replaced by one
        // silence word covering the whole file.
        if (words.empty()) {
            return srt::Expected<void>();
        }

        // A silence word is inserted before the first word, into every gap between words, and
        // after the last word.
        std::vector<Word> result;
        result.reserve(words.size() + 2);

        const auto silenceWord = [&label](const double start, const double duration) {
            Word silence{};
            silence.text = label;
            silence.start = start;
            silence.duration = duration;
            silence.phones.push_back(Phone{label, start, duration});
            return silence;
        };

        if (words.front().start > 0) {
            result.push_back(silenceWord(0, words.front().start));
        }

        result.push_back(words.front());

        // The gap is measured from the end of the last word already in the result, as in the
        // reference, which reads it from its growing list: a word that overlaps the previous word
        // therefore opens no gap, and a word that starts after its end does.
        for (std::size_t i = 1; i < words.size(); ++i) {
            const double cursor = result.back().start + result.back().duration;
            if (words[i].start > cursor) {
                result.push_back(silenceWord(cursor, words[i].start - cursor));
            }
            result.push_back(words[i]);
        }

        const double tailStart = result.back().start + result.back().duration;
        if (tailStart < wavLength) {
            result.push_back(silenceWord(tailStart, wavLength - tailStart));
        }

        words = std::move(result);
        return srt::Expected<void>();
    }

    void Aligner::clearLanguagePrefix(std::vector<Word> &words) {
        // The prefix is part of the phoneme labels, not of the words, and consists of the text up
        // to and including the last '/'. A label without '/' is left unchanged.
        for (Word &word : words) {
            for (Phone &phone : word.phones) {
                const std::size_t pos = phone.text.find_last_of('/');
                if (pos != std::string::npos) {
                    phone.text = phone.text.substr(pos + 1);
                }
            }
        }
    }

}
