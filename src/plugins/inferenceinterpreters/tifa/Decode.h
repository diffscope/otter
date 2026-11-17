#ifndef OTTER_INFERENCEINTERPRETERS_TIFA_DECODE_H
#define OTTER_INFERENCEINTERPRETERS_TIFA_DECODE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace otter::tifa {

    /// One token's place on the frame grid, half open: the frames from \c start up to \c end.
    ///
    /// Zero wide when the decode gave the token no frame at all, which is what the reference calls
    /// a skipped token and what its zero-width handling then removes from the result.
    struct Span {
        std::size_t start = 0;
        std::size_t end = 0;
    };

    /// The flat Viterbi of the reference decode, over one sample.
    ///
    /// \a similarities holds one row of scores per frame, as the model reports them: the raw
    /// cosine similarity of a token to that frame, not a probability. \a groups is each token's
    /// group number, and a pause is free between two tokens of different groups and forbidden
    /// between two of the same one.
    ///
    /// Returns one span per token, in the frame order the scores arrived in. A frame is spent
    /// either on the token it scores or on a pause, so a token's span is the run of frames it took
    /// and a pause leaves a hole rather than a label: what to call that hole is the caller's, not
    /// the decode's. A token no frame reached is reported zero wide, at the first pause position
    /// the groups allow from it on: the reference re-anchors every run of dropped tokens there and
    /// splits the run at it, which a caller that reads only a width cannot tell apart but one that
    /// reads the anchor can. The skip penalty and the zero-width handling are the exporter's
    /// defaults, as the contract carries no knob for either.
    std::vector<Span> decodeFrames(const std::vector<float> &similarities, std::size_t frames,
                                   std::size_t tokens, const std::vector<std::int64_t> &groups);

}

#endif // OTTER_INFERENCEINTERPRETERS_TIFA_DECODE_H
