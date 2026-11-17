#ifndef OTTER_INFERENCEINTERPRETERS_TIFA_SCORING_H
#define OTTER_INFERENCEINTERPRETERS_TIFA_SCORING_H

#include <array>
#include <cstdint>
#include <vector>

#include <synthrt/Support/Expected.h>

/// The whole-word half of the tifa text side: the fragment table the score graph reports, and the
/// dynamic program that picks one candidate per word over it.
namespace otter::tifa {

    /// The fragment table one phrase's score graph reported.
    ///
    /// A fragment is a run of template rows the host decided share a choice: the slots one
    /// candidate moves as a unit, and every array below has one row per fragment, the graph's own
    /// padding fragment first. The row and column counts come from the caller, which is what read
    /// the tensors: the table holds no shape of its own to disagree with the data.
    struct Fragments {
        /// The word and the segment of each fragment, as the graph's descriptors: the word is what
        /// ties a fragment to the word it belongs to, 0 marking the padding fragment.
        std::vector<std::array<std::int64_t, 2>> descriptors;

        /// How many of a fragment's rows each candidate fills: [fragments, candidates].
        std::vector<std::int64_t> lengths;

        /// The log probability a candidate scored in a fragment, by the offset that candidate has
        /// already spent in the fragment's segment: [fragments, candidates, fragments].
        std::vector<float> costs;

        /// The SPACE suffix of a segment, by the offset spent in it: [fragments, fragments].
        std::vector<float> tails;

        /// The MASK slots of each segment, as the graph wrote them: [reported].
        std::vector<std::int64_t> capacity;

        std::size_t fragments = 0;
        std::size_t candidates = 0;
    };

    /// The MASK slots of every segment, indexed by segment id, with the padding state at zero.
    ///
    /// The exporter's own graph writes one entry per fragment with the padding fragment first, so
    /// entry s belongs to segment s and entry 0 is the zero the padding row carries. A graph that
    /// leaves that entry out numbers the same counts from the first segment, and says so itself:
    /// the count of a segment that exists is never zero, so an array whose first value is not zero
    /// is one that starts at segment one rather than at padding.
    std::vector<std::int64_t> segmentSlots(const Fragments &table);

    /// Picks one candidate per word, the whole-word dynamic program of the exporter's inference
    /// script ported onto \a table.
    ///
    /// The answer is a candidate id per word, numbered from one, with zero for a word the run left
    /// unread, which is what the select graph reads. \a valid is each word's candidates, one flag
    /// per column, prefix packed the way the grid holds them. A word with no valid candidate keeps
    /// its zero and walks the fragments of no candidate, exactly as the reference does when a word
    /// has no pronunciation to score.
    ///
    /// Nothing of the reference's conditional scores is computed here: the exporter keeps them for
    /// callers that rank candidates against each other, and this contract asks only which one won.
    srt::Expected<std::vector<std::int64_t>>
        chooseCandidates(const Fragments &table, const std::vector<std::vector<bool>> &valid);

}

#endif // OTTER_INFERENCEINTERPRETERS_TIFA_SCORING_H
