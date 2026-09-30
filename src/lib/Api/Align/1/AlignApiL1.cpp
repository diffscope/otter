#include <otter/Api/Align/1/AlignApiL1.h>

#include <algorithm>
#include <string>
#include <utility>

#include <synthrt/Support/JSON.h>

#include <otter/Support/ManifestValues.h>

namespace otter::Api::Align::L1 {

    namespace {

        // Reads one entry of the languages array: the language, the scheme of its phonemes, the
        // form of its lyrics, and the phonemes a result can contain.
        srt::Expected<LanguageInfo> readLanguageInfo(const srt::JsonValue &value) {
            if (!value.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "each entry of languages must be an object");
            }
            const auto object = value.toObject();
            if (auto checked = manifest::rejectUnknownKeys(
                    object, {"language", "scheme", "lyrics", "phonemes"}, "a languages entry");
                !checked) {
                return checked.takeError();
            }
            LanguageInfo entry;
            for (const char *key : {"language", "scheme", "lyrics", "phonemes"}) {
                if (object.find(key) == object.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      std::string("a languages entry requires ") + key);
                }
            }
            auto language = manifest::readLanguage(object.at("language"), "language");
            if (!language) {
                return language.takeError();
            }
            entry.language = language.take();
            auto scheme = manifest::readScheme(object.at("scheme"), "scheme");
            if (!scheme) {
                return scheme.takeError();
            }
            entry.scheme = scheme.take();
            auto lyrics = manifest::readString(object.at("lyrics"), "lyrics");
            if (!lyrics) {
                return lyrics.takeError();
            }
            if (*lyrics == "scheme") {
                entry.lyrics = LyricsForm::Scheme;
            } else if (*lyrics == "text") {
                entry.lyrics = LyricsForm::Text;
            } else {
                return srt::Error(srt::Error::InvalidFormat,
                                  "lyrics must be scheme or text, and " + *lyrics + " is neither");
            }
            auto phonemes = manifest::readStringList(object.at("phonemes"), "phonemes");
            if (!phonemes) {
                return phonemes.takeError();
            }
            if (phonemes->empty()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the languages entry of " + entry.language + " " + entry.scheme +
                                      " lists no phonemes");
            }
            entry.phonemes = phonemes.take();
            return entry;
        }

        srt::Expected<std::vector<LanguageInfo>> readLanguages(const srt::JsonValue &value) {
            if (!value.isArray()) {
                return srt::Error(srt::Error::InvalidFormat, "languages must be an array");
            }
            std::vector<LanguageInfo> result;
            for (const auto &item : value.toArray()) {
                auto entry = readLanguageInfo(item);
                if (!entry) {
                    return entry.takeError();
                }
                for (const auto &seen : result) {
                    if (seen.language == entry->language && seen.scheme == entry->scheme) {
                        return srt::Error(srt::Error::InvalidFormat,
                                          "languages contains " + seen.language + " " +
                                              seen.scheme + " twice");
                    }
                }
                result.push_back(entry.take());
            }
            return result;
        }

    }

    srt::Expected<std::unique_ptr<AlignSchema>> readAlignSchema(const srt::ContribSpec &spec,
                                                                std::string variant) {
        const auto &value = spec.manifestExports();
        if (!value.isObject()) {
            return srt::Error(srt::Error::InvalidFormat,
                              "an Align declaration needs an exports object that specifies the "
                              "accepted audio format and the honored knobs");
        }
        const auto object = value.toObject();
        if (auto checked =
                manifest::rejectUnknownKeys(object,
                                            {"sampleRate", "channelCount", "maxSegmentDuration",
                                             "languages", "defaultLanguage", "nonSpeechPhonemes",
                                             "defaultNonSpeechPhonemes", "silenceLabel", "knobs"},
                                            "the Align exports");
            !checked) {
            return checked.takeError();
        }

        auto result = std::make_unique<AlignSchema>(std::move(variant));

        const auto rate = object.find("sampleRate");
        if (rate == object.end()) {
            return srt::Error(srt::Error::InvalidFormat, "the Align exports need a sampleRate");
        }
        if (auto number = manifest::readPositiveInt(rate->second, "sampleRate"); number) {
            result->sampleRate = number.take();
        } else {
            return number.takeError();
        }

        if (const auto it = object.find("channelCount"); it != object.end()) {
            auto number = manifest::readPositiveInt(it->second, "channelCount");
            if (!number) {
                return number.takeError();
            }
            result->channelCount = number.take();
        }
        if (const auto it = object.find("maxSegmentDuration"); it != object.end()) {
            auto number = manifest::readPositiveDouble(it->second, "maxSegmentDuration");
            if (!number) {
                return number.takeError();
            }
            result->maxSegmentDuration = number.take();
        }
        if (const auto it = object.find("languages"); it != object.end()) {
            auto languages = readLanguages(it->second);
            if (!languages) {
                return languages.takeError();
            }
            result->languages = languages.take();
        }
        if (const auto it = object.find("defaultLanguage"); it != object.end()) {
            auto code = manifest::readLanguage(it->second, "defaultLanguage");
            if (!code) {
                return code.takeError();
            }
            result->defaultLanguage = code.take();
        }
        // A host may omit the language, and the declaration must specify the language used in
        // that case.
        if (!result->languages.empty() && result->defaultLanguage.empty()) {
            return srt::Error(srt::Error::InvalidFormat,
                              "the Align exports list languages but declare no defaultLanguage");
        }
        if (!result->defaultLanguage.empty() &&
            std::none_of(result->languages.begin(), result->languages.end(),
                         [&result](const LanguageInfo &entry) {
                             return entry.language == result->defaultLanguage;
                         })) {
            return srt::Error(srt::Error::InvalidFormat,
                              "the Align exports declare " + result->defaultLanguage +
                                  " as the default language, which is not a listed language");
        }
        if (const auto it = object.find("nonSpeechPhonemes"); it != object.end()) {
            auto list = manifest::readStringList(it->second, "nonSpeechPhonemes");
            if (!list) {
                return list.takeError();
            }
            result->nonSpeechPhonemes = list.take();
        }
        if (const auto it = object.find("defaultNonSpeechPhonemes"); it != object.end()) {
            auto list = manifest::readStringList(it->second, "defaultNonSpeechPhonemes");
            if (!list) {
                return list.takeError();
            }
            for (const auto &label : *list) {
                if (std::find(result->nonSpeechPhonemes.begin(), result->nonSpeechPhonemes.end(),
                              label) == result->nonSpeechPhonemes.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the default non-speech phoneme " + label +
                                          " is not among the declared nonSpeechPhonemes");
                }
            }
            result->defaultNonSpeechPhonemes = list.take();
        }
        if (const auto it = object.find("silenceLabel"); it != object.end()) {
            // The key is optional by design: a module that names silence declares the label, and
            // a module that omits the key does not report silence as a word.
            auto label = manifest::readString(it->second, "silenceLabel");
            if (!label) {
                return label.takeError();
            }
            result->silenceLabel = label.take();
        }

        if (const auto it = object.find("knobs"); it != object.end()) {
            if (!it->second.isObject()) {
                return srt::Error(srt::Error::InvalidFormat, "knobs must be an object");
            }
            const auto knobs = it->second.toObject();
            if (auto checked = manifest::rejectUnknownKeys(
                    knobs, {"nonSpeechThreshold", "nonSpeechMinDuration", "gapFill"},
                    "the Align knobs");
                !checked) {
                return checked.takeError();
            }
            const std::pair<const char *, Common::L1::Knob AlignSchema::*> continuous[] = {
                {"nonSpeechThreshold",   &AlignSchema::nonSpeechThreshold  },
                {"nonSpeechMinDuration", &AlignSchema::nonSpeechMinDuration},
                {"gapFill",              &AlignSchema::gapFill             },
            };
            for (const auto &[key, member] : continuous) {
                const auto knob = knobs.find(key);
                if (knob == knobs.end()) {
                    continue;
                }
                auto read = manifest::readKnob(knob->second, key);
                if (!read) {
                    return read.takeError();
                }
                result.get()->*member = read.take();
            }
        }
        return result;
    }

}
