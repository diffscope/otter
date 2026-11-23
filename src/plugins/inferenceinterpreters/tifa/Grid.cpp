#include "Grid.h"

#include <algorithm>
#include <sstream>

#include <onnxutil/OnnxSupport.h>

namespace otter::tifa {

    namespace {

        // The language separator the model's symbols carry, as in <tt>zh/a</tt>.
        constexpr char LANGUAGE_SEPARATOR = '/';

        // One cell of an aligned profile: which token of that row's candidate sits there, numbered
        // from one, or zero where the candidate has no token in that slot.
        using Slot = std::size_t;

        // A word's candidates laid out over shared slots: one row per slot, one entry per
        // candidate.
        using Profile = std::vector<std::vector<Slot>>;

        // Where a candidate's token ends up once it is merged into a profile.
        struct Merge {
            // Whether the slot takes the next token of every candidate already in the profile, or
            // is one only the new candidate reaches.
            std::vector<bool> shares;

            // The new candidate's token at each slot, numbered from one: zero for a slot it does
            // not reach.
            std::vector<Slot> added;
        };

        // Aligns \a candidate against the profile's consensus.
        //
        // The cost is zero for equal tokens and one for everything else, and the walk back prefers
        // a match to a deletion and a deletion to an insertion. That preference is not a detail:
        // it is what decides which of two equally distant alignments of a repeated token comes
        // out, and the two put the token in different slots.
        Merge alignTo(const std::vector<std::int64_t> &consensus,
                      const std::vector<std::int64_t> &candidate) {
            const std::size_t n = consensus.size();
            const std::size_t m = candidate.size();
            std::vector<std::vector<std::int64_t>> distances(n + 1,
                                                             std::vector<std::int64_t>(m + 1, 0));
            for (std::size_t i = 0; i <= n; ++i) {
                distances[i][0] = static_cast<std::int64_t>(i);
            }
            for (std::size_t j = 0; j <= m; ++j) {
                distances[0][j] = static_cast<std::int64_t>(j);
            }
            for (std::size_t i = 1; i <= n; ++i) {
                for (std::size_t j = 1; j <= m; ++j) {
                    const auto replace =
                        distances[i - 1][j - 1] + (consensus[i - 1] == candidate[j - 1] ? 0 : 1);
                    distances[i][j] =
                        std::min({replace, distances[i - 1][j] + 1, distances[i][j - 1] + 1});
                }
            }

            Merge merge;
            std::size_t i = n;
            std::size_t j = m;
            while (i > 0 || j > 0) {
                const bool matches =
                    i > 0 && j > 0 &&
                    distances[i][j] ==
                        distances[i - 1][j - 1] + (consensus[i - 1] == candidate[j - 1] ? 0 : 1);
                if (matches) {
                    merge.shares.push_back(true);
                    merge.added.push_back(j);
                    --i;
                    --j;
                } else if (i > 0 && distances[i][j] == distances[i - 1][j] + 1) {
                    merge.shares.push_back(true);
                    merge.added.push_back(0);
                    --i;
                } else {
                    merge.shares.push_back(false);
                    merge.added.push_back(j);
                    --j;
                }
            }
            std::reverse(merge.shares.begin(), merge.shares.end());
            std::reverse(merge.added.begin(), merge.added.end());
            return merge;
        }

        // The token of \a candidate at \a slot of \a profile, or zero where it has none.
        std::int64_t tokenAt(const Profile &profile,
                             const std::vector<std::vector<std::int64_t>> &numbers,
                             std::size_t candidate, std::size_t slot) {
            const auto cell = profile[candidate][slot];
            return cell == 0 ? 0 : numbers[candidate][cell - 1];
        }

        // The profile's own reading of each slot: the token most of its candidates hold there.
        //
        // It is a reference to align the next candidate against rather than a decision: a slot the
        // candidates disagree about is still one slot, and which token they disagree about is what
        // the aligner walks along. A tie keeps the token of the earliest candidate that reaches
        // the slot, which is what makes the whole profile a function of the dictionary's own
        // order.
        std::vector<std::int64_t>
            consensusOf(const Profile &profile,
                        const std::vector<std::vector<std::int64_t>> &numbers) {
            std::vector<std::int64_t> consensus;
            if (profile.empty()) {
                return consensus;
            }
            for (std::size_t slot = 0; slot < profile.front().size(); ++slot) {
                std::vector<std::int64_t> counts;
                std::vector<std::size_t> seen;
                for (std::size_t candidate = 0; candidate < profile.size(); ++candidate) {
                    const auto token = tokenAt(profile, numbers, candidate, slot);
                    if (token == 0) {
                        continue;
                    }
                    const auto found = std::find(seen.begin(), seen.end(), token);
                    if (found == seen.end()) {
                        seen.push_back(token);
                        counts.push_back(1);
                    } else {
                        ++counts[static_cast<std::size_t>(std::distance(seen.begin(), found))];
                    }
                }
                const auto most = std::max_element(counts.begin(), counts.end());
                consensus.push_back(
                    seen[static_cast<std::size_t>(std::distance(counts.begin(), most))]);
            }
            return consensus;
        }

        // Lays every candidate of one word out over the slots they share.
        //
        // The first candidate's tokens are the slots to begin with, and every later one is aligned
        // against what the earlier ones agree on, which is how a slot keeps meaning the same place
        // in the word as candidates are added rather than becoming an artifact of the pairing
        // order. Both sides expand at once: a candidate's token the profile does not have takes a
        // new slot, and the candidates already in it take a gap there.
        Profile alignCandidates(const std::vector<std::vector<std::int64_t>> &numbers) {
            Profile profile;
            if (numbers.empty()) {
                return profile;
            }
            profile.push_back({});
            for (std::size_t token = 0; token < numbers.front().size(); ++token) {
                profile.front().push_back(token + 1);
            }
            for (std::size_t candidate = 1; candidate < numbers.size(); ++candidate) {
                const auto merge = alignTo(consensusOf(profile, numbers), numbers[candidate]);
                Profile merged(profile.size() + 1);
                for (std::size_t row = 0; row < profile.size(); ++row) {
                    std::size_t cell = 0;
                    for (const auto shares : merge.shares) {
                        merged[row].push_back(shares ? profile[row][cell++] : 0);
                    }
                }
                merged.back() = merge.added;
                profile = std::move(merged);
            }
            return profile;
        }

    }

    srt::Expected<Vocabulary> Vocabulary::read(const std::filesystem::path &path) {
        auto document = otter::onnx::readJsonObject(path, "the model's vocabulary.json");
        if (!document) {
            return document.takeError();
        }
        const auto root = document.take().toObject();
        const auto symbols = root.find("symbols");
        if (symbols == root.end() || !symbols->second.isObject()) {
            return srt::Error(srt::Error::InvalidFormat,
                              "the model's vocabulary.json carries no symbols object");
        }
        Vocabulary result;
        for (const auto &[label, index] : symbols->second.toObject()) {
            // A repeated number is not a fault: the exporter gives a symbol that two languages
            // merge, one phoneme with two spellings, a single number, and both spellings are
            // what the text side looks up.
            if (!index.isInt() || index.toInt() < 0) {
                return srt::Error(srt::Error::InvalidFormat, "the vocabulary entry " + label +
                                                                 " is not a non-negative integer");
            }
            result.m_symbols.emplace(label, index.toInt());
        }
        if (result.m_symbols.empty()) {
            return srt::Error(srt::Error::InvalidFormat, "the model's vocabulary is empty");
        }
        return result;
    }

    std::int64_t Vocabulary::id(const std::string &label) const {
        const auto it = m_symbols.find(label);
        return it == m_symbols.end() ? 0 : it->second;
    }

    srt::Expected<Dictionary> Dictionary::load(const std::filesystem::path &path) {
        auto text = otter::onnx::readTextFile(path, "the pronunciation dictionary");
        if (!text) {
            return text.takeError();
        }

        Dictionary result;
        std::istringstream lines(text.take());
        std::string line;
        while (std::getline(lines, line)) {
            // A trailing carriage return is not part of the written unit: dictionaries carry CRLF
            // as readily as LF, and the two must produce the same table.
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }

            const auto tab = line.find('\t');
            if (tab == std::string::npos) {
                // A line without a tab separates no phonemes from its written unit, so it can be
                // told apart neither from a unit that is missing nor from one whose entry is
                // empty. Shipping such a line would hide the fault as a word that is left unsung.
                return srt::Error(srt::Error::InvalidFormat,
                                  "the dictionary line \"" + line +
                                      "\" separates no phonemes from its written unit");
            }

            std::istringstream phonemesStr(line.substr(tab + 1));
            std::vector<std::string> phonemes;
            std::string phoneme;
            while (phonemesStr >> phoneme) {
                phonemes.push_back(phoneme);
            }
            result.m_entries[line.substr(0, tab)].push_back(std::move(phonemes));
        }
        return result;
    }

    const std::vector<std::vector<std::string>> *Dictionary::find(const std::string &unit) const {
        const auto it = m_entries.find(unit);
        return it == m_entries.end() ? nullptr : &it->second;
    }

    srt::Expected<Grid> buildGrid(std::string_view lyrics, const std::string &code,
                                  const Dictionary &dictionary, const Vocabulary &vocabulary) {
        Grid grid;
        // A written unit is read as every candidate the dictionary gives that the vocabulary can
        // spell from end to end. The upstream dictionaries and the vocabulary they were built
        // against are not exactly the same set: a handful of their entries are sung with a phoneme
        // the vocabulary has no symbol for, and the number a missing symbol looks up to is zero:
        // the exporter's padding, which is not a phoneme at all. Reading such a candidate would
        // hand the model a padding slot in the middle of a phrase and produce a confident alignment
        // of the wrong word, so an unspellable candidate is no candidate, exactly as a written unit
        // with no entry at all is: the word keeps the readings that did resolve, the words that did
        // resolve still align, and a phrase none of whose words resolved is refused by the caller,
        // which is where the difference can still be reported.
        std::vector<std::vector<std::vector<std::int64_t>>> numbers;
        std::vector<std::vector<std::vector<std::string>>> readings;
        std::vector<std::vector<std::vector<std::string>>> phonemes;
        std::vector<std::string> texts;
        std::istringstream wordsStr{std::string(lyrics)};
        std::string written;
        while (wordsStr >> written) {
            const auto *candidates = dictionary.find(written);
            if (candidates == nullptr) {
                continue;
            }

            std::vector<std::vector<std::int64_t>> wordNumbers;
            std::vector<std::vector<std::string>> wordSpellings;
            std::vector<std::vector<std::string>> wordPhonemes;
            for (const auto &candidate : *candidates) {
                if (candidate.empty()) {
                    continue;
                }
                std::vector<std::int64_t> candidateNumbers;
                std::vector<std::string> candidateSpellings;
                std::vector<std::string> candidatePhonemes;
                for (const auto &phoneme : candidate) {
                    // The dictionary writes a phoneme the way its language speaks it. The
                    // vocabulary is what decides how the model spells it. Follow the reference's
                    // \c Vocabulary.resolve: a bare symbol in the vocabulary wins, and a symbol
                    // that already names a language is never prefixed a second time.
                    std::string label = phoneme;
                    auto number = vocabulary.id(label);
                    if (number == 0 && phoneme.find(LANGUAGE_SEPARATOR) == std::string::npos) {
                        label = code + LANGUAGE_SEPARATOR + phoneme;
                        number = vocabulary.id(label);
                    }
                    if (number == 0) {
                        candidateNumbers.clear();
                        break;
                    }
                    candidateNumbers.push_back(number);
                    candidateSpellings.push_back(std::move(label));
                    candidatePhonemes.push_back(phoneme);
                }
                if (candidateNumbers.size() != candidate.size()) {
                    continue;
                }
                wordNumbers.push_back(std::move(candidateNumbers));
                wordSpellings.push_back(std::move(candidateSpellings));
                wordPhonemes.push_back(std::move(candidatePhonemes));
            }
            if (wordNumbers.empty()) {
                continue;
            }
            numbers.push_back(std::move(wordNumbers));
            readings.push_back(std::move(wordSpellings));
            phonemes.push_back(std::move(wordPhonemes));
            texts.push_back(written);
        }

        grid.texts = std::move(texts);
        grid.readings = std::move(readings);
        grid.spellings.reserve(phonemes.size());
        for (const auto &word : phonemes) {
            grid.spellings.push_back(word.front());
        }

        // The columns are the candidates a word can have, and the grid holds as many as the widest
        // word: the candidates a word does have are its first columns, which is the prefix packing
        // the exporter's own encoding writes and the score graph reads.
        std::size_t columns = 1;
        for (const auto &word : numbers) {
            columns = std::max(columns, word.size());
        }
        grid.candidates.assign(grid.texts.size(), std::vector<bool>(columns, false));
        for (std::size_t word = 0; word < numbers.size(); ++word) {
            for (std::size_t candidate = 0; candidate < numbers[word].size(); ++candidate) {
                grid.candidates[word][candidate] = true;
            }
        }

        for (std::size_t word = 0; word < numbers.size(); ++word) {
            const auto profile = alignCandidates(numbers[word]);
            // The group numbers say which phones were one written unit: every phone of a unit is
            // one group, so a pause is free between written units and forbidden inside one. A
            // candidate's own reading of the unit is what it holds at a slot. Where it holds
            // nothing, no group claims the slot.
            const auto unit = static_cast<std::int64_t>(word) + 1;
            for (std::size_t slot = 0; slot < profile.front().size(); ++slot) {
                std::vector<std::int64_t> rowNumbers(columns, 0);
                std::vector<std::int64_t> rowGroups(columns, 0);
                for (std::size_t candidate = 0; candidate < numbers[word].size(); ++candidate) {
                    const auto cell = profile[candidate][slot];
                    if (cell == 0) {
                        continue;
                    }
                    rowNumbers[candidate] = numbers[word][candidate][cell - 1];
                    rowGroups[candidate] = unit;
                }
                // A row every candidate leaves empty would be padding to the text graphs, which is
                // what a row of no tokens means, and the slot would drop out of the phrase without
                // a word of it being lost. The alignment never builds one, and a fault that did
                // would be a hole in the template rather than a shorter phrase.
                if (std::all_of(rowNumbers.begin(), rowNumbers.end(),
                                [](std::int64_t number) { return number == 0; })) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the pronunciation of " + grid.texts[word] +
                                          " aligned to a slot no reading fills");
                }
                grid.paths.push_back(std::move(rowNumbers));
                grid.groups.push_back(std::move(rowGroups));
                grid.words.push_back(unit);
            }
        }
        return grid;
    }

}
