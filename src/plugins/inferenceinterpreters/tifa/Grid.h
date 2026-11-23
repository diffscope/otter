#ifndef OTTER_INFERENCEINTERPRETERS_TIFA_GRID_H
#define OTTER_INFERENCEINTERPRETERS_TIFA_GRID_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <synthrt/Support/Expected.h>
#include <synthrt/Support/Json.h>

/// The text side of the tifa aligner: the model's own symbol table, the pronunciation dictionaries
/// the declaration names, and the candidate grid the model's graphs consume.
namespace otter::tifa {

    /// The phoneme symbols one model can emit, as the exporter spells them.
    ///
    /// The table is the model's own: a symbol carries the language code it belongs to
    /// (<tt>zh/a</tt>, <tt>en/aa</tt>) or no language at all, which is how the exporter spells a
    /// symbol every language may use. The numbers are what the graphs speak, and 0 is reserved for
    /// padding rather than being a symbol, so a lookup that fails yields it.
    class Vocabulary {
    public:
        /// Reads the exporter's vocabulary.json, which holds a symbols object that maps each label
        /// to its identifier.
        static srt::Expected<Vocabulary> read(const std::filesystem::path &path);

        /// The number of \a label, or 0 when the vocabulary does not carry it.
        std::int64_t id(const std::string &label) const;

        /// Every symbol the vocabulary carries, in its own file's order.
        const std::map<std::string, std::int64_t> &symbols() const {
            return m_symbols;
        }

    private:
        std::map<std::string, std::int64_t> m_symbols;
    };

    /// One language's pronunciation dictionary: a written unit, and the phonemes it is sung as.
    ///
    /// The file is the exporter's own, one entry per line: the written unit, a tab, then its
    /// phonemes separated by spaces. A unit written on more than one line is sung more than one
    /// way: the lines are the candidates, in the file's order.
    class Dictionary {
    public:
        static srt::Expected<Dictionary> load(const std::filesystem::path &path);

        /// The candidates of \a unit, or null when the dictionary carries none.
        const std::vector<std::vector<std::string>> *find(const std::string &unit) const;

    private:
        std::unordered_map<std::string, std::vector<std::vector<std::string>>> m_entries;
    };

    /// The candidate grid one phrase and one language make.
    ///
    /// This is the shape the model's text graphs are built around rather than a convenience: the
    /// rows are the slots a candidate can hold a phoneme in, the columns are the candidates, and
    /// every row names the word it belongs to and the group it is in. The group numbers are what
    /// tells the decode where a pause may fall: the phones of one written unit are one group and
    /// stay contiguous, and the word numbers are what aggregates the decoded phones back onto
    /// the words the caller wrote.
    ///
    /// A word's candidates are aligned against one another rather than listed one after another, so
    /// a slot belongs to a word rather than to a candidate: a column holds a candidate's phoneme
    /// where it has one and a gap where it does not. That is what lets one set of rows describe
    /// every reading of a word, and what the score graph numbers its segments over.
    ///
    /// The rows of the grid are the rows of the template the model is asked about, and the
    /// invariant that keeps the two in step is that a row carries at least one token: a row every
    /// candidate leaves empty would read as padding to the text graphs, whose padding is what a row
    /// of no tokens means, and the word it belongs to would lose the slot in silence. Nothing here
    /// builds such a row, because a row exists only where some candidate's pronunciation reaches.
    struct Grid {
        /// Phoneme numbers, one column per candidate. Id 0 is a candidate's gap, and the exporter's
        /// padding where the column holds no candidate at all.
        std::vector<std::vector<std::int64_t>> paths;

        /// The word each row belongs to, numbered from one. No row is padding.
        std::vector<std::int64_t> words;

        /// Group numbers, one column per candidate, 0 where that candidate has no token in the row.
        std::vector<std::vector<std::int64_t>> groups;

        /// Whether a candidate of a word exists at all, prefix packed as the exporter packs it: the
        /// candidates a word has are its first columns and the rest are false.
        std::vector<std::vector<bool>> candidates;

        /// The vocabulary's own spelling of every candidate's phonemes, in the order that candidate
        /// sings them. This is where a result's labels come from, and it is per candidate rather
        /// than per cell because the select graph compacts: it removes the rows a chosen reading
        /// leaves empty, so a row of its output is not the grid row of the same number and the
        /// cells a row came from are not recoverable from the row's own number. What is recoverable
        /// is the order: a reading's phonemes keep their order however the rows move, so the k-th
        /// row the graph kept for a word is the k-th phoneme of the reading that word was read as.
        std::vector<std::vector<std::vector<std::string>>> readings;

        /// The words the caller wrote, in order, and the words the dictionary resolved.
        std::vector<std::string> texts;

        /// The phonemes each text's first candidate spells, which is the reading the template's own
        /// first column carries.
        std::vector<std::vector<std::string>> spellings;

        std::size_t wordCount() const {
            return texts.size();
        }

        std::size_t columns() const {
            return paths.empty() ? 0 : paths.front().size();
        }
    };

    /// Expands \p lyrics into the grid the model's graphs consume.
    ///
    /// The lyrics are split on whitespace and every word is looked up in \a dictionary. A word the
    /// dictionary does not carry is left out, which is what the exporter's own default does with an
    /// unknown word: the model aligns the words that resolved, and a result padded with silence in
    /// their place would read as words absent from the audio rather than as a failure to understand
    /// them. A word none of whose readings the vocabulary can spell is left out the
    /// same way, because a symbol the vocabulary does not carry is not a phoneme and could only be
    /// read as the padding between them. A phrase none of whose words resolved is refused by the
    /// caller, which is where that difference can still be reported.
    ///
    /// A written unit the dictionary spells more than one way is one word with one candidate per
    /// reading, aligned against one another by their own edit distance: the columns share the rows
    /// where the readings agree and branch where they do not, which is the grid the whole-word
    /// score picks a reading out of. A reading the vocabulary cannot spell from end to end is no
    /// reading at all and is dropped, so the candidates a word has are the ones the model could be
    /// asked about.
    ///
    /// \a code is the model's own code for the language, which is what the vocabulary prefixes the
    /// symbols of that language with.
    srt::Expected<Grid> buildGrid(std::string_view lyrics, const std::string &code,
                                  const Dictionary &dictionary, const Vocabulary &vocabulary);

}

#endif // OTTER_INFERENCEINTERPRETERS_TIFA_GRID_H
