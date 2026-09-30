#include "G2p.h"

#include <fstream>
#include <sstream>
#include <utility>

#include <stdcorelib/path.h>

namespace otter::hfa {

    G2p::G2p(std::unordered_map<std::string, std::vector<std::string>> entries)
        : m_entries(std::move(entries)) {
    }

    srt::Expected<G2p> G2p::load(const std::filesystem::path &path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            return srt::Error(srt::Error::FileNotOpen, "cannot open the pronunciation dictionary " +
                                                           stdc::path::to_utf8(path));
        }

        std::unordered_map<std::string, std::vector<std::string>> entries;
        std::string line;
        while (std::getline(file, line)) {
            // A trailing carriage return is not part of the word: dictionaries use CRLF as often
            // as LF, and both line endings must produce the same table.
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }

            const auto tab = line.find('\t');
            if (tab == std::string::npos) {
                // A line without a tab specifies no phonemes, so the word has no entry to resolve
                // to. The reference drops such a line. Unlike a missing word, a dropped line cannot
                // be distinguished from a word with an empty entry, so a shipped file containing
                // such a line would hide the defect silently; the line is therefore rejected.
                return srt::Error(srt::Error::InvalidFormat,
                                  "the dictionary line \"" + line +
                                      "\" separates no phonemes from its word");
            }

            std::istringstream phonemesStr(line.substr(tab + 1));
            std::vector<std::string> phonemes;
            std::string phoneme;
            while (phonemesStr >> phoneme) {
                phonemes.push_back(phoneme);
            }

            entries[line.substr(0, tab)] = std::move(phonemes);
        }

        return G2p(std::move(entries));
    }

    Phrase G2p::split(const std::string &lyrics, const std::string &separator) const {
        std::istringstream lyricsStr(lyrics);
        std::vector<std::string> words;
        std::string word;
        while (lyricsStr >> word) {
            words.push_back(word);
        }

        Phrase phrase;
        phrase.phonemes.push_back(separator);
        phrase.phonemeToWord.push_back(-1);

        for (const auto &written : words) {
            const auto entry = m_entries.find(written);
            if (entry == m_entries.end()) {
                // The word is excluded from the phrase instead of reaching the aligner as an
                // unresolvable word. A word absent from the dictionary is omitted: the alignment
                // then covers the audio with the resolved words, and the caller can detect the
                // omission in Phrase::words.
                continue;
            }

            const auto &phonemes = entry->second;
            const auto wordIndex = static_cast<int>(phrase.words.size());
            phrase.words.push_back(written);

            const auto before = phrase.phonemes.size();
            for (std::size_t i = 0; i < phonemes.size(); ++i) {
                if ((i == 0 || i == phonemes.size() - 1) && phonemes[i] == separator) {
                    // A separator inside the word's entry would be interpreted as the gap between
                    // words instead of as a phoneme of the word.
                    continue;
                }
                phrase.phonemes.push_back(phonemes[i]);
                phrase.phonemeToWord.push_back(wordIndex);
            }

            if (phrase.phonemes.size() > before && phrase.phonemes.back() != separator) {
                phrase.phonemes.push_back(separator);
                phrase.phonemeToWord.push_back(-1);
            }
        }

        return phrase;
    }
}
