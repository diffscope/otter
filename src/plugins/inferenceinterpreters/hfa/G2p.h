#ifndef OTTER_HFA_G2P_H
#define OTTER_HFA_G2P_H

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <synthrt/Support/Expected.h>

#include "Aligner.h"  // otter::hfa::Phrase

namespace otter::hfa {

    /// The pronunciation dictionary of one language, which maps each word to its phonemes.
    class G2p {
    public:
        /// Reads a dictionary file with one entry per line: the word, a tab, then its phonemes
        /// separated by spaces.
        ///
        /// \return The dictionary; \c srt::Error::FileNotOpen if the file cannot be opened;
        ///         \c srt::Error::InvalidFormat if a non-empty line contains no tab.
        static srt::Expected<G2p> load(const std::filesystem::path &path);

        /// Splits \p lyrics on whitespace, looks up every word, and returns the phoneme sequence
        /// with \p separator before the first word, between words and after the last word.
        ///
        /// \p separator is the declaration's silence label, which the provider has verified to be
        /// one of the model's silent classes. A separator at the beginning or end of an entry is
        /// removed from the entry, because it would be interpreted as the gap between two words.
        /// \sa otter::hfa::Phrase
        Phrase split(const std::string &lyrics, const std::string &separator) const;

    private:
        /// Takes ownership of \p entries, so that a dictionary read by \c load cannot be observed
        /// partially built.
        explicit G2p(std::unordered_map<std::string, std::vector<std::string>> entries);

        std::unordered_map<std::string, std::vector<std::string>> m_entries;
    };

}

#endif // OTTER_HFA_G2P_H
