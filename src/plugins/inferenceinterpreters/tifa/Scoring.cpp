#include "Scoring.h"

#include <algorithm>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <utility>

namespace otter::tifa {

    namespace {

        // One place the walk can be: the segment it last entered, and how many of that segment's
        // MASK slots it has spent. The offset is what the table is indexed by, so it is part of
        // the state rather than a running total: two candidates that reach a segment with a
        // different number of slots spent are in different places.
        struct State {
            std::int64_t segment = 0;
            std::int64_t used = 0;

            bool operator==(const State &other) const {
                return segment == other.segment && used == other.used;
            }
        };

        struct StateHash {
            std::size_t operator()(const State &state) const {
                return std::hash<std::int64_t>{}(state.segment) ^
                       (std::hash<std::int64_t>{}(state.used) * 1099511628211u);
            }
        };

        // What reached one state: the total the path scored, the state it stepped from and the
        // candidate it took: the parent the choices are read back through.
        struct Arrival {
            State state;
            double total = 0.0;
            State source;
            std::int64_t candidate = 0;

            // How a tie is broken: the rank of the state stepped from, then the candidate. Which
            // of two paths that scored the same wins is then a property of the grid rather than of
            // the order the table happened to be walked in.
            std::pair<std::int64_t, std::int64_t> key;
        };

        // The MASK slots of a segment, which is how many rows the graph reported for it.
        std::int64_t slotsOf(const std::vector<std::int64_t> &slots, std::int64_t segment) {
            return segment < static_cast<std::int64_t>(slots.size()) ? slots[segment] : 0;
        }

        // Whether the table describes a grid the walk can be run on at all.
        //
        // The table is the graph's own account of a grid the caller handed over, so the two can
        // disagree: a fragment that names a word or a segment the tensors do not carry would be
        // read past its own end, and one that holds more slots than a segment's offset axis has
        // places for would be indexed outside it. Refusing beats indexing past the end of either.
        srt::Expected<void> checkTable(const Fragments &table,
                                       const std::vector<std::vector<bool>> &valid) {
            if (table.fragments == 0 || table.candidates == 0 ||
                table.descriptors.size() != table.fragments ||
                table.lengths.size() != table.fragments * table.candidates ||
                table.costs.size() != table.fragments * table.candidates * table.fragments ||
                table.tails.size() != table.fragments * table.fragments || table.capacity.empty()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the score graph returned a fragment table of no known shape");
            }
            for (const auto &descriptor : table.descriptors) {
                if (descriptor[0] < 0 || descriptor[0] > static_cast<std::int64_t>(valid.size())) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the scoring template names the word " +
                                          std::to_string(descriptor[0]) +
                                          ", which the grid does not carry");
                }
                if (descriptor[1] < 0 ||
                    descriptor[1] >= static_cast<std::int64_t>(table.fragments)) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the scoring template names the segment " +
                                          std::to_string(descriptor[1]) +
                                          ", which the fragment table does not carry");
                }
            }
            const auto slots = segmentSlots(table);
            for (std::size_t segment = 0; segment < slots.size(); ++segment) {
                if (slots[segment] < 0 ||
                    slots[segment] >= static_cast<std::int64_t>(table.fragments)) {
                    return srt::Error(
                        srt::Error::InvalidFormat,
                        "the scoring template gives the segment " + std::to_string(segment) + " " +
                            std::to_string(slots[segment]) + " slots and the fragment table has " +
                            std::to_string(table.fragments) + " offsets");
                }
            }
            for (const auto &row : valid) {
                if (row.size() > table.candidates) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the grid offers " + std::to_string(row.size()) +
                                          " candidates and the fragment table scores " +
                                          std::to_string(table.candidates));
                }
            }
            return srt::Expected<void>();
        }

        // Walks every fragment a candidate owns, and reports the state and the score it left the
        // walk in, or nothing when the candidate does not fit the segment it is walking.
        //
        // A candidate is never scored fragment by fragment on its own: the slots it spends are
        // what the next fragment's offset is read at, so the walk carries the state through all of
        // them. The SPACE suffix is charged once, where the walk leaves a segment for another, and
        // the last one where the phrase ends.
        std::optional<std::pair<State, double>>
            advance(State from, const std::vector<std::size_t> &pieces, std::size_t candidate,
                    const Fragments &table, const std::vector<std::int64_t> &slots) {
            double score = 0.0;
            for (const auto fragment : pieces) {
                const auto current = table.descriptors[fragment][1];
                if (current != from.segment) {
                    score += table.tails[from.segment * table.fragments + from.used];
                    from = {current, 0};
                }
                const auto length = table.lengths[fragment * table.candidates + candidate];
                if (from.used + length > slotsOf(slots, from.segment)) {
                    return std::nullopt;
                }
                score += table.costs[(fragment * table.candidates + candidate) * table.fragments +
                                     from.used];
                from.used += length;
            }
            return std::make_pair(from, score);
        }

        // Numbers the states of one layer in the order the ties of the next layer are broken in.
        //
        // The sort is stable over the layer's own order, which is what makes the run repeatable
        // when two states carry the same key.
        std::unordered_map<State, std::int64_t, StateHash>
            numberStates(const std::vector<Arrival> &layer) {
            std::vector<std::size_t> order(layer.size());
            std::iota(order.begin(), order.end(), 0);
            std::stable_sort(order.begin(), order.end(),
                             [&layer](std::size_t left, std::size_t right) {
                                 return layer[left].key < layer[right].key;
                             });
            std::unordered_map<State, std::int64_t, StateHash> ranks;
            for (std::size_t rank = 0; rank < order.size(); ++rank) {
                ranks.emplace(layer[order[rank]].state, static_cast<std::int64_t>(rank));
            }
            return ranks;
        }

    }

    std::vector<std::int64_t> segmentSlots(const Fragments &table) {
        const bool fromFirstSegment = !table.capacity.empty() && table.capacity.front() != 0;
        std::vector<std::int64_t> slots(table.capacity.size() + 1, 0);
        for (std::size_t entry = 0; entry < table.capacity.size(); ++entry) {
            slots[fromFirstSegment ? entry + 1 : entry] = table.capacity[entry];
        }
        return slots;
    }

    srt::Expected<std::vector<std::int64_t>>
        chooseCandidates(const Fragments &table, const std::vector<std::vector<bool>> &valid) {
        if (auto checked = checkTable(table, valid); !checked) {
            return checked.takeError();
        }
        const auto slots = segmentSlots(table);

        // The word each fragment belongs to, so the walk of a word does not search the table per
        // word. Only a fragment that names both a word and a segment is walked: one that names a
        // word and no segment has rows the readings share rather than rows a word is walked over,
        // and the padding fragment's zero word, like the numbering of the counts, is read from the
        // values rather than assumed (see segmentSlots in Scoring.h).
        std::vector<std::vector<std::size_t>> pieces(valid.size());
        for (std::size_t fragment = 0; fragment < table.fragments; ++fragment) {
            const auto word = table.descriptors[fragment][0];
            if (word > 0 && table.descriptors[fragment][1] > 0 &&
                word <= static_cast<std::int64_t>(valid.size())) {
                pieces[static_cast<std::size_t>(word) - 1].push_back(fragment);
            }
        }

        // One layer per word: the states the phrases scored so far end in. The layer in front of
        // the first word is the single state of a phrase nothing has been read of yet.
        std::vector<std::vector<Arrival>> layers;
        layers.push_back({
            Arrival{State{}, 0.0, State{}, 0, {0, 0}}
        });
        std::vector<std::unordered_map<State, std::int64_t, StateHash>> ranks;
        ranks.push_back({
            {State{}, 0}
        });

        for (std::size_t word = 0; word < valid.size(); ++word) {
            std::vector<std::int64_t> candidates;
            for (std::size_t column = 0; column < valid[word].size(); ++column) {
                if (valid[word][column]) {
                    candidates.push_back(static_cast<std::int64_t>(column) + 1);
                }
            }
            // A word with no candidate at all is not read: it walks the fragments of no candidate
            // and keeps the choice zero. That is a word the grid holds and the vocabulary cannot
            // spell, which is a different thing from a word that is not in the grid.
            if (candidates.empty()) {
                candidates.push_back(0);
            }

            std::vector<Arrival> following;
            std::unordered_map<State, std::size_t, StateHash> at;
            for (const auto &source : layers.back()) {
                const auto rank = ranks.back().at(source.state);
                for (const auto candidate : candidates) {
                    const auto transition =
                        candidate == 0
                            ? std::optional<std::pair<State, double>>(
                                  std::make_pair(source.state, 0.0))
                            : advance(source.state, pieces[word],
                                      static_cast<std::size_t>(candidate) - 1, table, slots);
                    if (!transition) {
                        continue;
                    }
                    const auto &[target, cost] = *transition;
                    const auto total = source.total + cost;
                    const auto key = std::make_pair(rank, candidate);
                    const auto found = at.find(target);
                    if (found == at.end()) {
                        at.emplace(target, following.size());
                        following.push_back({target, total, source.state, candidate, key});
                    } else if (auto &entry = following[found->second];
                               total > entry.total || (total == entry.total && key < entry.key)) {
                        entry.total = total;
                        entry.source = source.state;
                        entry.candidate = candidate;
                        entry.key = key;
                    }
                }
            }
            // A word whose every candidate fails to fit the template is a grid the graph's own
            // table does not describe, since the candidate the grid was built from is one of them.
            if (following.empty()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "no complete pronunciation path fits the scoring template");
            }
            ranks.push_back(numberStates(following));
            layers.push_back(std::move(following));
        }

        // The phrase ends where it ends: the last segment is closed at the offset the winning path
        // stopped at, and among the paths that scored the same the lowest ranked state wins.
        const auto &last = layers.back();
        std::size_t winner = 0;
        double best =
            last.front().total +
            table.tails[last.front().state.segment * table.fragments + last.front().state.used];
        std::int64_t bestRank = ranks.back().at(last.front().state);
        for (std::size_t entry = 1; entry < last.size(); ++entry) {
            const auto total =
                last[entry].total +
                table.tails[last[entry].state.segment * table.fragments + last[entry].state.used];
            const auto rank = ranks.back().at(last[entry].state);
            if (total > best || (total == best && rank < bestRank)) {
                best = total;
                bestRank = rank;
                winner = entry;
            }
        }

        std::vector<std::int64_t> choices(valid.size(), 0);
        std::size_t reached = winner;
        for (std::size_t word = valid.size(); word-- > 0;) {
            const auto &step = layers[word + 1][reached];
            choices[word] = step.candidate;
            const auto &previous = layers[word];
            const auto source =
                std::find_if(previous.begin(), previous.end(),
                             [&step](const Arrival &entry) { return entry.state == step.source; });
            // Every state a layer holds was reached from one the layer before it holds, so a state
            // that is missing there is a table the walk built out of two different ones.
            if (source == previous.end()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the scoring template's layers do not join");
            }
            reached = static_cast<std::size_t>(std::distance(previous.begin(), source));
        }
        return choices;
    }

}
