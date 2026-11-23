#include "Decode.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace otter::tifa {

    namespace {

        // What dropping a token costs.
        //
        // The reference charges this on the similarity scale rather than on a log probability
        // scale, which only means anything because the scores stay on that scale throughout: a
        // token is worth dropping only when the fit of the tokens around it gains more than half
        // a cosine similarity by the room it frees. It is the reference's \c skip_penalty, and 0.5
        // is exact in a \c float, so the states below stay on \c float and keep the reference's
        // rounding as well as its decisions.
        constexpr float SKIP_PENALTY = 0.5f;

        // No way to have arrived here at all.
        constexpr float UNREACHABLE = -std::numeric_limits<float>::infinity();

        // How a pause position was reached, as the reference's \c gap_back says it: a skip is a
        // token dropped at no cost in frames, an exit is the token in front of the position
        // handing the frame back to it, and a wait is a frame spent on the pause there.
        constexpr std::int8_t GAP_WAIT = 0;
        constexpr std::int8_t GAP_EXIT = 1;
        constexpr std::int8_t GAP_SKIP = 2;

        // How a token was reached, as the reference's \c token_back says it: from its own state on
        // the frame before, or from the pause in front of it.
        constexpr std::int8_t TOKEN_SELF = 0;
        constexpr std::int8_t TOKEN_FROM_GAP = 1;

        // A frame index no walk has placed a token at.
        constexpr std::int64_t UNSET = -1;

    }

    std::vector<Span> decodeFrames(const std::vector<float> &similarities, std::size_t frames,
                                   std::size_t tokens, const std::vector<std::int64_t> &groups) {
        std::vector<Span> spans(tokens);
        if (frames == 0 || tokens == 0) {
            return spans;
        }

        // A position counts the tokens that have been dealt with, so 0 is in front of the phrase
        // and \c tokens is behind it. Both of those are places a pause may sit in. Inside the
        // phrase it is a group boundary that decides, because the tokens of one written unit are
        // one group and a pause between two of them is not a pause but a misalignment. The
        // reference builds the same table the same way and calls it \c gap_allowed.
        const std::size_t positions = tokens + 1;
        const auto allowed = [&groups, tokens](std::size_t position) {
            return position == 0 || position == tokens || groups[position - 1] != groups[position];
        };

        // The objective is the summed similarity of the frames a token took minus the penalty for
        // every token it did not take, and the states are what a position is worth under it: the
        // pause there, and the token in front of it sounding. Dropping a token costs its penalty
        // and no frame, which is why a skip is resolved inside a step rather than taking one of
        // its own.
        //
        // A frame index is a place between frames: 0 is in front of the first one and \c frames is
        // behind the last, and the reference reads its states the same way round. Walking back
        // needs to know how every state was reached, which is what the back pointer arrays carry,
        // and they are written frame by frame as the reference writes them rather than rebuilt
        // afterwards, so a state overwritten later keeps the pointer of the value that won.
        std::vector<std::int8_t> gapBack((frames + 1) * positions, GAP_WAIT);
        std::vector<std::int8_t> tokenBack(frames * tokens, TOKEN_SELF);
        std::vector<float> gap(positions), token(tokens);
        std::vector<float> nextGap(positions), nextToken(tokens);

        // In front of the first frame nothing has sounded, and every token in front of a pause has
        // been dropped at its penalty: the skipped prefix the reference charges for, and the back
        // pointers it leaves behind say so.
        gap[0] = 0.0f;
        for (std::size_t i = 0; i < tokens; ++i) {
            gap[i + 1] = gap[i] - SKIP_PENALTY;
            gapBack[i + 1] = GAP_SKIP;
            token[i] = UNREACHABLE;
        }

        for (std::size_t frame = 1; frame <= frames; ++frame) {
            const auto *scores = similarities.data() + (frame - 1) * tokens;
            const auto tokenRow = (frame - 1) * tokens;
            const auto gapRow = frame * positions;

            // A token sounds on this frame either because it was already sounding or because the
            // pause in front of it spent the frame here. Ties keep the token: the reference only
            // leaves the pause when the pause is strictly the better of the two.
            for (std::size_t i = 0; i < tokens; ++i) {
                tokenBack[tokenRow + i] = TOKEN_SELF;
                float best = token[i];
                if (gap[i] > best) {
                    best = gap[i];
                    tokenBack[tokenRow + i] = TOKEN_FROM_GAP;
                }
                nextToken[i] = best + scores[i];
            }

            // A pause the frame is spent on carries no more than the position in front of it did.
            // A position whose group boundary may hold no pause has no such move at all, and is
            // left unreachable here rather than forbidden later.
            std::fill(nextGap.begin(), nextGap.end(), UNREACHABLE);
            for (std::size_t i = 0; i <= tokens; ++i) {
                if (allowed(i)) {
                    nextGap[i] = gap[i];
                }
            }

            // The two ways a token hands a pause the value back, both of them inside the frame and
            // neither of them spending one: an exit, which costs nothing, and a skip in front of
            // it, which costs the penalty and is how a token that never sounded got passed. The
            // positions are walked forwards so that what a skip leaves at one is offered to the
            // next as well, and on a tie the exit wins and the skip is not taken, because the
            // reference only takes the skip when it is strictly better, which is the same order
            // that makes a token worth exactly its penalty be emitted rather than dropped.
            for (std::size_t i = 0; i < tokens; ++i) {
                if (nextToken[i] > nextGap[i + 1]) {
                    nextGap[i + 1] = nextToken[i];
                    gapBack[gapRow + i + 1] = GAP_EXIT;
                }
                const float skipped = nextGap[i] - SKIP_PENALTY;
                if (skipped > nextGap[i + 1]) {
                    nextGap[i + 1] = skipped;
                    gapBack[gapRow + i + 1] = GAP_SKIP;
                }
            }

            gap.swap(nextGap);
            token.swap(nextToken);
        }

        // The walk starts behind the last frame and behind the phrase, on the pause there, and
        // follows the pointers back from it. It cannot start anywhere else: every token has either
        // been emitted or been dropped in front of that pause, and a token dropped at the end of
        // the phrase costs its penalty exactly as one dropped in the middle does. So a phrase whose
        // trailing tokens nothing emitted is worth less than it looks, and one that simply ends at
        // its best scoring position is not the alignment the reference reports.
        std::vector<std::int64_t> start(tokens, UNSET);
        std::vector<std::int64_t> end(tokens, UNSET);
        std::int64_t frame = static_cast<std::int64_t>(frames);
        std::int64_t position = static_cast<std::int64_t>(tokens);
        bool inToken = false;
        // Both indices are signed because each of them is stepped past its own floor to leave the
        // loop: the frame index in front of the first frame, once the skipped prefix is being
        // walked, and the position in front of the first token. A position that is stepped over is
        // also the number of the token it names from then on: the token in front of a position is
        // the one the position is named for, so \a position is read as a token index once the
        // walk has stepped into a token or over one, and as a pause position otherwise.
        while (frame > 0 || position > 0 || inToken) {
            if (inToken) {
                // The frames of a run are visited from behind, so the end of the span is taken once
                // and the start keeps moving back for as long as the token was already sounding.
                const auto index = static_cast<std::size_t>(position);
                if (end[index] == UNSET) {
                    end[index] = frame;
                }
                start[index] = frame - 1;
                const auto entered =
                    tokenBack[(static_cast<std::size_t>(frame) - 1) * tokens + index];
                --frame;
                if (entered == TOKEN_FROM_GAP) {
                    inToken = false;
                }
            } else {
                const auto source = gapBack[static_cast<std::size_t>(frame) * positions +
                                            static_cast<std::size_t>(position)];
                if (source == GAP_WAIT) {
                    --frame;
                } else if (source == GAP_EXIT) {
                    // The token in front gave this frame back, so the walk is inside it now and the
                    // position moves into it before the frame is accounted for.
                    --position;
                    inToken = true;
                } else {
                    // A dropped token takes no frame, so the walk steps over it where it stands,
                    // zero wide, and the anchoring below decides where that is read from.
                    --position;
                    const auto index = static_cast<std::size_t>(position);
                    start[index] = frame;
                    end[index] = frame;
                }
            }
        }

        // A run of dropped tokens is zero wide wherever the walk left it, and the reference moves
        // each run to the first pause position the groups allow, splitting the run there: what
        // stands in front of that position is held at the end of the span before the run and the
        // rest at the start of the span behind it. A pause the groups forbid is not a place a drop
        // may be read as standing in for, so a run whose own positions are all forbidden is held
        // entirely on the boundary behind it.
        std::size_t lo = 0;
        while (lo < tokens) {
            if (start[lo] != end[lo]) {
                ++lo;
                continue;
            }
            std::size_t hi = lo;
            while (hi + 1 < tokens && start[hi + 1] == end[hi + 1]) {
                ++hi;
            }
            const std::int64_t left = lo > 0 ? end[lo - 1] : 0;
            const std::int64_t right =
                hi + 1 < tokens ? start[hi + 1] : static_cast<std::int64_t>(frames);
            std::size_t anchorAt = lo;
            while (anchorAt <= hi + 1 && !allowed(anchorAt)) {
                ++anchorAt;
            }
            for (std::size_t i = lo; i <= hi; ++i) {
                const std::int64_t anchor = i < anchorAt ? left : right;
                start[i] = anchor;
                end[i] = anchor;
            }
            lo = hi + 1;
        }

        for (std::size_t token = 0; token < tokens; ++token) {
            if (start[token] != UNSET) {
                spans[token] = {static_cast<std::size_t>(start[token]),
                                static_cast<std::size_t>(end[token])};
            }
        }
        return spans;
    }

}
