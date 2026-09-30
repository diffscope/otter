#ifndef OTTER_HFA_ALIGNER_H
#define OTTER_HFA_ALIGNER_H

/// \file Aligner.h
/// The decode step of the hubert-infer forced aligner: a three-state Viterbi over phoneme frame
/// logits plus per-frame edge logits, and the word list bookkeeping around it.
///
/// Ported from the reference implementation's AlignmentDecoder and WordList
/// (dataset-tools/src/engine/engines/hubert-infer). The arithmetic is a direct transliteration
/// of the reference, so this file deliberately keeps the reference's structure (the
/// `log`/`exp`/`sigmoid` calls, the clip constants and, above all, the integer frame arithmetic)
/// even if a simpler formulation exists. Boundaries are compared numerically against the reference
/// binary, and a simplified formula that shifts a boundary by one frame would be a defect.

#include <map>
#include <string>
#include <vector>

#include <synthrt/Support/Expected.h>

namespace otter::hfa {

    /// One phoneme with the span it covers.
    struct Phone {
        std::string text;
        double start = 0;     ///< Seconds.
        double duration = 0;  ///< Seconds.
    };

    /// One word with the span it covers and the phonemes inside it.
    struct Word {
        std::string text;
        double start = 0;                                ///< Seconds.
        double duration = 0;                             ///< Seconds.
        std::vector<Phone> phones;
    };

    /// The phoneme sequence produced from a line of lyrics, and the word index of each phoneme
    /// (-1 for the separators that the splitter inserts between words).
    struct Phrase {
        std::vector<std::string> phonemes;
        std::vector<std::string> words;
        std::vector<int> phonemeToWord;
    };

    /// Converts phoneme frame logits and edge logits into word and phone boundaries.
    ///
    /// Every span produced here is a half-open interval [start, start + duration): the decoder
    /// emits one boundary per frame index and takes the difference between neighbours, so adjacent
    /// spans share an identical boundary value. The word list operations below depend on this
    /// property, because they splice words by comparing boundaries exactly; a lossy conversion of
    /// the boundaries would misalign the joins.
    class Aligner {
    public:
        /// \param vocabulary phoneme label -> class index, as declared by the model's vocab.json
        /// \param separator the label that the splitter inserts between words; it must be in
        ///        \a vocabulary, and the decode treats its class as silence
        /// \param hopSize samples between adjacent frames
        /// \param sampleRate rate the frames were computed at
        Aligner(std::map<std::string, int> vocabulary, std::string separator, int hopSize,
                int sampleRate);

        /// Returns the number of whole frames that \a wavLength seconds of audio cover, computed
        /// in the same way as in the decode.
        int frameCount(double wavLength) const;

        /// Decodes the words and phones of \p phrase from the model output.
        ///
        /// \param phFrameLogits batch 0 of the model output, laid out [classes][frames]; taken by
        ///        value because the decode masks and trims it in place, so a caller that no longer
        ///        needs it moves it in instead of copying it
        /// \param phEdgeLogits one edge logit per frame, taken by value for the same reason
        /// \param wavLength length of the audio in seconds; audio shorter than one frame is
        ///        rejected, because no word can be placed in it
        /// \return The decoded words, or \c srt::Error::InvalidArgument if a phoneme of \p phrase
        ///         is not in the vocabulary, the audio is shorter than one frame, the logits are
        ///         empty or shorter than the audio, or the decode yields more phonemes than frames.
        ///
        /// The separators are excluded from the words because they represent the gaps between
        /// words.
        srt::Expected<std::vector<Word>> decode(std::vector<std::vector<float>> phFrameLogits,
                                                std::vector<float> phEdgeLogits,
                                                const Phrase &phrase, double wavLength) const;

        /// Inserts \p entry, a non-speech span detected by the model, into \p words.
        ///
        /// If \p entry overlaps no word, it is inserted unchanged. Otherwise every part of
        /// \p entry that overlaps a word is removed, the words stay unchanged, and each remaining
        /// piece of at least \p minimumDuration seconds is inserted.
        ///
        /// \return An empty value, or \c srt::Error::InvalidArgument if \p entry has no phones.
        static srt::Expected<void> insertNonSpeech(std::vector<Word> &words, const Word &entry,
                                                   double minimumDuration);

        /// Absorbs short gaps into the adjacent words: a leading gap shorter than \p gapFill
        /// seconds into the first word, and every other gap of at most \p gapFill seconds into
        /// the word before it.
        static void fillSmallGaps(std::vector<Word> &words, double wavLength, double gapFill);

        /// Fills every remaining gap, including the gaps before the first word and after the last
        /// word, with a word labelled \p label.
        ///
        /// \return An empty value in every case; an empty \p words is left unchanged.
        static srt::Expected<void> addSilence(std::vector<Word> &words, double wavLength,
                                              const std::string &label);

        /// Removes the language prefix from the phoneme labels, as the reference's
        /// clear_language_prefix does.
        static void clearLanguagePrefix(std::vector<Word> &words);

    private:
        std::map<std::string, int> m_vocabulary;
        std::string m_separator;

        /// The class of \c m_separator. The reference assumes class 0, which holds for the released
        /// vocabulary; reading the class from the vocabulary prevents a model with a different
        /// class numbering from being decoded against the wrong class.
        int m_separatorClass;

        int m_hopSize;
        int m_sampleRate;

        /// Duration of one frame in seconds, as a float, because the reference keeps it in float
        /// and every boundary is multiplied by it.
        float m_frameLength;
    };
}

#endif // OTTER_HFA_ALIGNER_H
