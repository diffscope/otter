// The forced alignment provider, ported from the HuBERT-FA engine in dataset-tools.
//
// HFA is a singing-voice forced aligner: it takes the sung lyrics as input and returns the words
// and phonemes it located, with the position of each in the audio. One graph performs all of the
// model's work (a HuBERT encoder, a phoneme classifier with an edge head, and a non-speech
// classifier), and all subsequent steps are decoding: a three-state Viterbi over the frame logits,
// the phoneme sequence into which the language's dictionary expands the lyrics, the breaths
// detected by the non-speech head, and the silence between them.
//
// This port differs from the original implementation in three respects. Times are in seconds,
// anchored at the start time that the host states for the audio, so the results of a host that
// analyzes slices are placed correctly. Audio arrives as PCM instead of as a file, because otter
// contains no audio code. The models form a spec 2.4 package whose declaration states the audio
// format, the languages and the knobs; the model's own files (its mel spectrogram config, its
// vocabulary and its dictionaries) are read through that declaration and checked against it
// before any analysis.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <stdcorelib/path.h>
#include <stdcorelib/plugin/plugin.h>

#include <synthrt/SVS/InferenceInterpreterPlugin.h>
#include <synthrt/Support/Expected.h>
#include <synthrt/Support/JSON.h>

#include <dsinfer/Core/Tensor.h>
#include <dsinfer/Inference/InferenceSession.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/AnalysisInput.h>
#include <otter/Analysis/AnalysisInterpreter.h>
#include <otter/Api/Align/1/AlignApiL1.h>
#include <otter/Support/ManifestValues.h>

#include "Aligner.h"
#include "G2p.h"
#include "NonSpeech.h"
#include "OnnxSupport.h"

namespace AlignApi = otter::Api::Align::L1;
namespace CommonApi = otter::Api::Common::L1;

namespace {

    /// The variant this provider implements.
    constexpr char VARIANT[] = "hfa";

    /// The values used if the declaration does not honor the corresponding knob.
    constexpr double FALLBACK_NON_SPEECH_THRESHOLD = 0.5;
    constexpr double FALLBACK_NON_SPEECH_MIN_DURATION = 0.1;
    constexpr double FALLBACK_GAP_FILL = 0.1;

    /// The tensor names of the exported graph, which form the contract between this variant and
    /// its model: waveform -> ph_frame_logits, ph_edge_logits, cvnt_logits.
    namespace tensors {
        constexpr char WAVEFORM[] = "waveform";
        constexpr char PH_FRAME_LOGITS[] = "ph_frame_logits";
        constexpr char PH_EDGE_LOGITS[] = "ph_edge_logits";
        constexpr char CVNT_LOGITS[] = "cvnt_logits";
    }

    /// The non-speech class that denotes the absence of every other non-speech class.
    ///
    /// The model numbers this class first. It is prepended here instead of being read from the
    /// vocabulary, as in the original implementation, because the frame scan requires the empty
    /// class at index zero.
    constexpr char NON_SPEECH_NONE[] = "None";

    /// The language separator the model's phoneme labels carry, as in "zh/a".
    constexpr char LANGUAGE_SEPARATOR = '/';

    using otter::onnx::TensorPtr;

    /// Reads a whole file as text.
    ///
    /// \return The text; \c srt::Error::FileNotOpen if the file cannot be opened;
    ///         \c srt::Error::InvalidFormat if the file is empty. \a what names the file in the
    ///         error.
    srt::Expected<std::string> readTextFile(const std::filesystem::path &path, std::string_view what) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return srt::Error(srt::Error::FileNotOpen,
                              "cannot open " + std::string(what) + " " +
                                  stdc::path::to_utf8(path));
        }
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.empty()) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " is empty: " + stdc::path::to_utf8(path));
        }
        return text;
    }

    /// Reads one of the model's own JSON files, as an object.
    ///
    /// These files belong to the model rather than to the package, and they are read with the
    /// spec's profile (UTF-8, comments allowed, repeated keys rejected). A document that is not an
    /// object is rejected instead of being read as empty.
    ///
    /// \return The object, or the error of the first failed step.
    srt::Expected<srt::JsonValue> readJsonObject(const std::filesystem::path &path,
                                                 std::string_view what) {
        auto text = readTextFile(path, what);
        if (!text) {
            return text.takeError();
        }
        stdc::json::ParseError problem;
        auto value = srt::JsonValue::fromJson(text.take(), true, &problem);
        if (problem) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " cannot be read: " + problem.message());
        }
        if (!value.isObject()) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " must hold a JSON object");
        }
        return value;
    }

    /// Reads an array of strings, empty ones included.
    ///
    /// manifest::readStringList is deliberately not used: the model's silent phoneme list begins
    /// with the empty label, which is a valid class name here.
    srt::Expected<std::vector<std::string>> readLabels(const srt::JsonValue &value,
                                                       std::string_view what) {
        if (!value.isArray()) {
            return srt::Error(srt::Error::InvalidFormat, std::string(what) + " must be an array");
        }
        std::vector<std::string> result;
        for (const auto &item : value.toArray()) {
            if (!item.isString()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  std::string(what) + " must hold strings");
            }
            result.push_back(item.toString());
        }
        return result;
    }

    /// The model properties recorded in the model's own files.
    struct ModelFacts {
        /// The sample rate of the graph's front end. A declaration stating another rate would make
        /// the host prepare audio that the model then misplaces.
        int sampleRate = 0;

        /// Samples between adjacent frames, which convert a frame index into a time.
        int hopSize = 0;

        /// The phoneme labels of the classifier, mapped to their class indices.
        std::map<std::string, int> vocabulary;

        /// The labels that the vocabulary classifies as silence rather than as sung phonemes.
        std::vector<std::string> silentPhonemes;

        /// The non-speech classes, beginning with the "None" class.
        std::vector<std::string> nonSpeechClasses;

        /// The dictionary file name per language code, as listed in the vocabulary.
        std::map<std::string, std::string> dictionaries;
    };

    /// Reads the mel spectrogram config and the vocabulary that a package's configuration
    /// references.
    ///
    /// \return The model properties, or \c srt::Error::InvalidFormat or the file error of the
    ///         first file or entry that cannot be read.
    srt::Expected<ModelFacts> readModelFacts(const std::filesystem::path &configPath,
                                             const std::filesystem::path &vocabPath) {
        auto configValue = readJsonObject(configPath, "the model's config.json");
        if (!configValue) {
            return configValue.takeError();
        }
        ModelFacts facts;
        {
            const auto config = configValue.take().toObject();
            const auto mel = config.find("mel_spec_config");
            if (mel == config.end() || !mel->second.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the model's config.json carries no mel_spec_config object");
            }
            const auto melSpec = mel->second.toObject();
            const auto read = [&melSpec](const char *key,
                                         int &out) -> srt::Expected<void> {
                const auto it = melSpec.find(key);
                if (it == melSpec.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      std::string("the model's mel_spec_config requires a ") + key);
                }
                auto number = otter::manifest::readPositiveInt(
                    it->second, std::string("mel_spec_config.") + key);
                if (!number) {
                    return number.takeError();
                }
                out = number.take();
                return srt::Expected<void>();
            };
            if (auto read0 = read("sample_rate", facts.sampleRate); !read0) {
                return read0.takeError();
            }
            if (auto read1 = read("hop_size", facts.hopSize); !read1) {
                return read1.takeError();
            }
        }

        auto vocabValue = readJsonObject(vocabPath, "the model's vocab.json");
        if (!vocabValue) {
            return vocabValue.takeError();
        }
        const auto vocab = vocabValue.take().toObject();

        const auto vocabulary = vocab.find("vocab");
        if (vocabulary == vocab.end() || !vocabulary->second.isObject()) {
            return srt::Error(srt::Error::InvalidFormat,
                              "the model's vocab.json carries no vocab object");
        }
        for (const auto &[label, index] : vocabulary->second.toObject()) {
            if (!index.isInt() || index.toInt() < 0) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the vocabulary entry " + label +
                                      " is not a non-negative integer");
            }
            facts.vocabulary.emplace(label, static_cast<int>(index.toInt()));
        }
        if (facts.vocabulary.empty()) {
            return srt::Error(srt::Error::InvalidFormat, "the model's vocabulary is empty");
        }

        const auto silent = vocab.find("silent_phonemes");
        if (silent == vocab.end()) {
            return srt::Error(srt::Error::InvalidFormat,
                              "the model's vocab.json carries no silent_phonemes list");
        }
        auto silentLabels = readLabels(silent->second, "silent_phonemes");
        if (!silentLabels) {
            return silentLabels.takeError();
        }
        facts.silentPhonemes = silentLabels.take();

        const auto nonSpeech = vocab.find("non_lexical_phonemes");
        if (nonSpeech == vocab.end()) {
            return srt::Error(srt::Error::InvalidFormat,
                              "the model's vocab.json carries no non_lexical_phonemes list");
        }
        auto nonSpeechLabels = readLabels(nonSpeech->second, "non_lexical_phonemes");
        if (!nonSpeechLabels) {
            return nonSpeechLabels.takeError();
        }
        facts.nonSpeechClasses.push_back(NON_SPEECH_NONE);
        for (auto &label : nonSpeechLabels.take()) {
            facts.nonSpeechClasses.push_back(std::move(label));
        }

        if (const auto it = vocab.find("dictionaries"); it != vocab.end()) {
            if (!it->second.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the model's dictionaries must be an object");
            }
            for (const auto &[code, name] : it->second.toObject()) {
                if (!name.isString() || name.toString().empty()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the dictionary of " + code + " must name a file");
                }
                facts.dictionaries.emplace(code, name.toString());
            }
        }
        return facts;
    }

    /// Returns whether the vocabulary classifies \a label as silence rather than as a sung phoneme.
    ///
    /// The result determines whether the language prefix is prepended to the label: the model's
    /// labels carry their language, and its silence classes belong to every language.
    bool isSilent(const std::vector<std::string> &silentPhonemes, const std::string &label) {
        return std::find(silentPhonemes.begin(), silentPhonemes.end(), label) !=
               silentPhonemes.end();
    }

    /// The configuration block of this variant, together with the contents of the model's own
    /// files.
    ///
    /// The block names the model and its accompanying files, and maps the host's language
    /// identifiers to the model's own codes. The audio format, the languages and the knobs are
    /// contract facts and are declared in exports. The model's files are read once, when the
    /// declaration is loaded, and stored here with the dictionaries, so that creating an analyzer
    /// reads only the graph.
    class HfaConfiguration : public srt::ContribConfiguration {
    public:
        HfaConfiguration()
            : srt::ContribConfiguration(AlignApi::API_INTERFACE, VARIANT, AlignApi::API_LEVEL) {
        }

        std::filesystem::path model;

        /// The model's own config.json, which specifies the mel spectrogram of its front end.
        std::filesystem::path config;

        /// The model's own vocab.json: the phoneme classes, the non-speech classes and the
        /// dictionaries.
        std::filesystem::path vocab;

        /// Maps the identifiers listed in the exports to the model's language codes.
        ///
        /// The contract uses identifiers because language codes are specific to each model: two
        /// models may use different codes for the same language, or the same code for different
        /// languages.
        std::map<std::string, std::string> languages;

        /// The model properties read from config.json and vocab.json.
        ModelFacts facts;

        /// One dictionary per language the exports declare, keyed by the host's identifier.
        ///
        /// Only the declared languages are loaded, because an execution cannot request any other
        /// language; a language that the configuration maps but the exports omit is not loaded.
        std::map<std::string, otter::hfa::G2p> dictionaries;
    };

    /// Reads a [1, classes, frames] tensor as one row of frames per class.
    ///
    /// Both classifiers return that shape, and the decode requires one row per class: the
    /// reference processes the classes of one frame at a time, and this layout allows the mask and
    /// the softmax over classes to operate on rows instead of gathering scattered values.
    ///
    /// \return One row per class, or \c AnalysisError::ModelFailed if \a tensor is not a float
    ///         tensor of shape [1, classes, frames] with a matching element count.
    srt::Expected<std::vector<std::vector<float>>> readClassFrames(const TensorPtr &tensor,
                                                                  const char *what) {
        if (!tensor || tensor->dataType() != ds::ITensor::Float) {
            return srt::Error(otter::AnalysisError::ModelFailed,
                              std::string(what) + " is not a float tensor");
        }
        const auto shape = tensor->shape();
        if (shape.size() != 3 || shape[0] != 1) {
            return srt::Error(otter::AnalysisError::ModelFailed,
                              std::string(what) + " is not a [1, classes, frames] tensor");
        }
        const auto classes = static_cast<std::size_t>(shape[1]);
        const auto frames = static_cast<std::size_t>(shape[2]);
        const auto view = tensor->view<float>();
        if (classes == 0 || frames == 0 || view.size() != classes * frames) {
            return srt::Error(otter::AnalysisError::ModelFailed,
                              std::string(what) + " carries " + std::to_string(view.size()) +
                                  " values, not " + std::to_string(classes * frames));
        }
        std::vector<std::vector<float>> result(classes, std::vector<float>(frames));
        for (std::size_t index = 0; index < classes; ++index) {
            std::copy_n(view.begin() + static_cast<std::ptrdiff_t>(index * frames), frames,
                        result[index].begin());
        }
        return result;
    }

    class HfaExecutive : public otter::onnx::OnnxExecutive<AlignApi::AlignExecutive> {
    public:
        HfaExecutive(srt::InferenceSpec &spec, std::unique_ptr<ds::InferenceSession> session,
                     const AlignApi::AlignSchema &schema, const HfaConfiguration &configuration)
            : OnnxExecutive(spec), m_session(own(std::move(session))), m_schema(schema),
              m_configuration(configuration) {
        }

        ~HfaExecutive() override {
            shutDown();
        }

    protected:
        srt::Expected<std::unique_ptr<AlignApi::AlignResult>>
            run(const AlignApi::AlignStartInput &input) override {
            const auto &audio = input.audio;
            auto prepared = otter::prepareSamples(audio, m_schema.sampleRate, m_schema.channelCount,
                                                  m_schema.maxSegmentDuration);
            if (!prepared) {
                return prepared.takeError();
            }
            const auto waveform = prepared.take();
            if (waveform.empty()) {
                return srt::Error(srt::Error::InvalidArgument, "the span carries no audio");
            }

            // An alignment without lyrics has nothing to align, and returning a transcription
            // instead would implement a different contract.
            if (input.lyrics.empty()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "an alignment requires the sung text, and lyrics is empty");
            }
            auto chosenLanguage = chooseLanguage(input);
            if (!chosenLanguage) {
                return chosenLanguage.takeError();
            }
            const auto &language = *chosenLanguage;
            auto labels = chooseLabels(input);
            if (!labels) {
                return labels.takeError();
            }
            auto chosen = chooseSettings(input);
            if (!chosen) {
                return chosen.takeError();
            }
            const auto settings = chosen.take();

            const auto report = [&input](double value) {
                if (input.progress) {
                    input.progress(value);
                }
            };
            // The length is computed as in the original implementation, by one float division of
            // the sample count by the rate, because the frame count is derived from the length. A
            // double that rounds differently in the last bit could move a boundary for a sample
            // count near a half frame.
            const auto &facts = m_configuration.facts;
            const double wavLength = static_cast<double>(static_cast<float>(waveform.size()) /
                                                         static_cast<float>(m_schema.sampleRate));
            const otter::hfa::Aligner aligner(facts.vocabulary, m_schema.silenceLabel,
                                              facts.hopSize, facts.sampleRate);
            // The audio is rejected before the model runs: no word fits in less than a frame, and
            // the decode would reject the audio after the most expensive step anyway.
            if (aligner.frameCount(wavLength) < 1) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "the audio is shorter than one frame of the model");
            }

            report(0.0);
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }

            // 1. Expand the lyrics through the dictionary.
            auto phrase = expand(input.lyrics, language);
            if (!phrase) {
                return phrase.takeError();
            }

            // 2. Run the graph over the whole span.
            auto logits = infer(waveform);
            if (!logits) {
                return logits.takeError();
            }
            report(0.5);

            // 3. Decode the words.
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }
            auto aligned = aligner.decode(std::move(logits->frames), std::move(logits->edges),
                                          *phrase, wavLength);
            if (!aligned) {
                return aligned.takeError();
            }
            std::vector<otter::hfa::Word> words = aligned.take();
            report(0.7);

            // 4. Insert the non-speech sounds into the gaps that the words leave.
            if (!labels->empty()) {
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }
                if (auto inserted = insertNonSpeech(words, std::move(logits->nonSpeech), *labels,
                                                    settings, wavLength);
                    !inserted) {
                    return inserted.takeError();
                }
            }
            report(0.85);

            // 5. Label the silence between the words. The reference absorbs the gaps that count as
            // a pause within a word before it labels the remaining gaps, and the reference results
            // depend on this order.
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }
            otter::hfa::Aligner::fillSmallGaps(words, wavLength, settings.gapFill);
            otter::hfa::Aligner::clearLanguagePrefix(words);
            if (auto filled =
                    otter::hfa::Aligner::addSilence(words, wavLength, m_schema.silenceLabel);
                !filled) {
                return filled.takeError();
            }

            // 6. Place the result on the host's timeline.
            auto result = place(std::move(words), *language.entry, audio.startTime);
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }
            report(1.0);
            return result;
        }

    private:
        /// The language of one execution: its declared entry, the model's code for it, and its
        /// dictionary.
        struct Language {
            const AlignApi::LanguageInfo *entry = nullptr;
            const std::string *code = nullptr;
            const otter::hfa::G2p *dictionary = nullptr;
        };

        /// The knob values of one execution.
        struct Settings {
            double threshold = 0;
            double minimumDuration = 0;
            double gapFill = 0;
        };

        /// The three classifier outputs, laid out as the decoders read them.
        struct Logits {
            std::vector<std::vector<float>> frames;
            std::vector<float> edges;
            std::vector<std::vector<float>> nonSpeech;
        };

        /// Finds the declared language and scheme that an execution requests.
        ///
        /// The scheme specifies the notation of the result's phonemes. A language may be declared
        /// with several schemes, in which case the caller must specify the scheme.
        ///
        /// \return The language entry, model code and dictionary;
        ///         \c srt::Error::InvalidArgument if no language is specified or declared, the
        ///         language or scheme is not declared, or the scheme is ambiguous;
        ///         \c AnalysisError::Internal if the dictionary is missing.
        srt::Expected<Language> chooseLanguage(const AlignApi::AlignStartInput &input) const {
            const std::string language = input.language.value_or(m_schema.defaultLanguage);
            if (language.empty()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this package declares no default language, so the caller must "
                                  "specify a language");
            }
            Language result;
            for (const auto &candidate : m_schema.languages) {
                if (candidate.language != language ||
                    (input.scheme && candidate.scheme != *input.scheme)) {
                    continue;
                }
                if (result.entry != nullptr) {
                    return srt::Error(srt::Error::InvalidArgument,
                                      "this package declares " + language +
                                          " with more than one scheme, so the caller must "
                                          "specify a scheme");
                }
                result.entry = &candidate;
            }
            if (result.entry == nullptr) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "the language " + language +
                                      (input.scheme ? " with the scheme " + *input.scheme : "") +
                                      " is not declared by this package");
            }
            const auto code = m_configuration.languages.find(language);
            if (code == m_configuration.languages.end()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "the language " + language + " is not declared by this package");
            }
            result.code = &code->second;
            const auto dictionary = m_configuration.dictionaries.find(language);
            if (dictionary == m_configuration.dictionaries.end()) {
                return srt::Error(otter::AnalysisError::Internal,
                                  "the package carries no dictionary for " + language);
            }
            result.dictionary = &dictionary->second;
            return result;
        }

        /// Returns the non-speech labels that an execution detects: the labels it names, or the
        /// declared defaults.
        ///
        /// \return The labels, or \c srt::Error::InvalidArgument if a label is not declared. An
        ///         undeclared label is rejected instead of ignored.
        srt::Expected<std::vector<std::string>>
            chooseLabels(const AlignApi::AlignStartInput &input) const {
            std::vector<std::string> labels = input.nonSpeechPhonemes.empty()
                                                  ? m_schema.defaultNonSpeechPhonemes
                                                  : input.nonSpeechPhonemes;
            for (const auto &label : labels) {
                if (std::find(m_schema.nonSpeechPhonemes.begin(), m_schema.nonSpeechPhonemes.end(),
                              label) == m_schema.nonSpeechPhonemes.end()) {
                    return srt::Error(srt::Error::InvalidArgument,
                                      "the caller asked for the non-speech phoneme " + label +
                                          ", which this package does not declare");
                }
            }
            return labels;
        }

        /// Chooses every knob of an execution, within the ranges that the declaration reports.
        ///
        /// \return The settings, or the error of the first knob outside its declared range.
        srt::Expected<Settings> chooseSettings(const AlignApi::AlignStartInput &input) const {
            Settings settings;
            const std::tuple<const std::optional<double> &, const CommonApi::Knob &, double,
                             double *, const char *>
                knobs[] = {
                    {input.nonSpeechThreshold,   m_schema.nonSpeechThreshold,
                     FALLBACK_NON_SPEECH_THRESHOLD,                                                &settings.threshold,       "nonSpeechThreshold"},
                    {input.nonSpeechMinDuration, m_schema.nonSpeechMinDuration,
                     FALLBACK_NON_SPEECH_MIN_DURATION,                                             &settings.minimumDuration,
                     "nonSpeechMinDuration"                                                                                                       },
                    {input.gapFill,              m_schema.gapFill,              FALLBACK_GAP_FILL, &settings.gapFill,
                     "gapFill"                                                                                                                    },
            };
            for (const auto &[given, knob, fallback, target, what] : knobs) {
                auto chosen = otter::chooseKnob(given, knob, fallback, what);
                if (!chosen) {
                    return chosen.takeError();
                }
                *target = chosen.take();
            }
            return settings;
        }

        /// Expands the lyrics into the model's phoneme labels.
        ///
        /// A word absent from the dictionary is skipped, as in the original implementation, and
        /// the model aligns the remaining words. If the dictionary contains none of the words,
        /// nothing remains, and a result consisting only of silence would indicate that the lyrics
        /// are absent from the audio instead of indicating that the words could not be resolved.
        ///
        /// \return The phrase, or \c srt::Error::InvalidArgument if no word is in the dictionary.
        srt::Expected<otter::hfa::Phrase> expand(const std::string &lyrics,
                                                 const Language &language) const {
            otter::hfa::Phrase phrase = language.dictionary->split(lyrics, m_schema.silenceLabel);
            for (auto &phoneme : phrase.phonemes) {
                if (!isSilent(m_configuration.facts.silentPhonemes, phoneme)) {
                    phoneme = *language.code + LANGUAGE_SEPARATOR + phoneme;
                }
            }
            if (phrase.words.empty()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "none of the lyrics is in this language's dictionary");
            }
            return phrase;
        }

        /// Runs the graph over the whole span.
        ///
        /// \return The three classifier outputs, or the error of the model call or of a malformed
        ///         output.
        srt::Expected<Logits> infer(const otter::PreparedSamples &waveform) const {
            otter::onnx::Tensors inputs;
            {
                auto tensor = otter::onnx::waveformTensor(waveform);
                if (!tensor) {
                    return tensor.takeError();
                }
                inputs[tensors::WAVEFORM] = tensor.take();
            }
            auto ran =
                runModel(*m_session, std::move(inputs),
                         {tensors::PH_FRAME_LOGITS, tensors::PH_EDGE_LOGITS, tensors::CVNT_LOGITS},
                         "hfa model");
            if (!ran) {
                return ran.takeError();
            }
            const auto outputs = ran.take();
            Logits result;
            auto frames =
                readClassFrames(outputs.at(tensors::PH_FRAME_LOGITS), tensors::PH_FRAME_LOGITS);
            if (!frames) {
                return frames.takeError();
            }
            result.frames = frames.take();
            auto edges = otter::onnx::readFloats(outputs.at(tensors::PH_EDGE_LOGITS),
                                                 tensors::PH_EDGE_LOGITS);
            if (!edges) {
                return edges.takeError();
            }
            result.edges = edges.take();
            auto nonSpeech =
                readClassFrames(outputs.at(tensors::CVNT_LOGITS), tensors::CVNT_LOGITS);
            if (!nonSpeech) {
                return nonSpeech.takeError();
            }
            result.nonSpeech = nonSpeech.take();
            return result;
        }

        /// Decodes the non-speech head and inserts the detected spans into the parts of the
        /// timeline that no word covers.
        ///
        /// \return An empty value, or the error of the decode or of an insertion.
        srt::Expected<void> insertNonSpeech(std::vector<otter::hfa::Word> &words,
                                            std::vector<std::vector<float>> logits,
                                            const std::vector<std::string> &labels,
                                            const Settings &settings, double wavLength) const {
            const auto &facts = m_configuration.facts;
            otter::hfa::NonSpeech decoder(facts.nonSpeechClasses, facts.hopSize, facts.sampleRate);
            decoder.setThreshold(settings.threshold);
            decoder.setMinimumDuration(settings.minimumDuration);
            auto spans = decoder.decode(std::move(logits), wavLength, labels);
            if (!spans) {
                return spans.takeError();
            }
            for (const auto &span : spans.take()) {
                if (auto inserted =
                        otter::hfa::Aligner::insertNonSpeech(words, span, settings.minimumDuration);
                    !inserted) {
                    return inserted.takeError();
                }
            }
            return srt::Expected<void>();
        }

        /// Places the words on the host's timeline. All model output is relative to the input
        /// span, and the contract's times are absolute, so the anchor is added once here.
        static std::unique_ptr<AlignApi::AlignResult> place(std::vector<otter::hfa::Word> words,
                                                            const AlignApi::LanguageInfo &entry,
                                                            double anchor) {
            auto result = std::make_unique<AlignApi::AlignResult>();
            result->language = entry.language;
            result->scheme = entry.scheme;
            result->words.reserve(words.size());
            for (auto &word : words) {
                AlignApi::WordInfo info;
                info.text = std::move(word.text);
                info.start = anchor + word.start;
                info.duration = word.duration;
                info.phones.reserve(word.phones.size());
                for (auto &phone : word.phones) {
                    AlignApi::PhoneInfo placed;
                    placed.text = std::move(phone.text);
                    placed.start = anchor + phone.start;
                    placed.duration = phone.duration;
                    info.phones.push_back(std::move(placed));
                }
                result->words.push_back(std::move(info));
            }
            return result;
        }

        ds::InferenceSession *m_session;

        /// Owned by the spec, which outlives every executive created from it.
        const AlignApi::AlignSchema &m_schema;
        const HfaConfiguration &m_configuration;
    };

    /// Reads the declaration and the model's own files, and creates analyzers.
    class HfaInterpreter : public otter::AnalysisInterpreter {
    public:
        HfaInterpreter()
            : AnalysisInterpreter(AlignApi::API_INTERFACE, AlignApi::API_LEVEL, VARIANT) {
        }

        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override {
            auto schema = AlignApi::readAlignSchema(spec, VARIANT);
            if (!schema) {
                return schema.takeError();
            }
            const auto &declared = **schema;
            if (declared.channelCount != 1) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "the hfa variant feeds its model one channel, and the exports "
                                  "declare " +
                                      std::to_string(declared.channelCount));
            }
            // The splitter inserts the silence label between the expanded words, and the decode
            // treats its class as silence, so the variant cannot run without a silence label.
            if (declared.silenceLabel.empty()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the hfa variant separates words with the silence label, and "
                                  "the exports declare none");
            }
            return std::unique_ptr<srt::ContribExports>(schema.take().release());
        }

        /// Reads the configuration block and the model's own files, and checks the declaration
        /// against them.
        ///
        /// Both the declaration and the model's files are available here, so every comparison
        /// between them is performed here, once, when the declaration is loaded. The exports are
        /// read again through the contract's reader instead of being taken from the spec, because
        /// the loader does not guarantee that the exports are interpreted first.
        ///
        /// \return The configuration, or the first error of reading or checking.
        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override {
            auto configuration = readConfiguration(spec);
            if (!configuration) {
                return configuration.takeError();
            }
            auto schema = AlignApi::readAlignSchema(spec, VARIANT);
            if (!schema) {
                return schema.takeError();
            }
            auto &wiring = **configuration;
            auto facts = readModelFacts(wiring.config, wiring.vocab);
            if (!facts) {
                return facts.takeError();
            }
            wiring.facts = facts.take();
            if (auto checked = checkAgainstModel(**schema, wiring); !checked) {
                return checked.takeError();
            }
            if (auto loaded = loadDictionaries(**schema, wiring); !loaded) {
                return loaded.takeError();
            }
            return std::unique_ptr<srt::ContribConfiguration>(configuration.take().release());
        }

        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override {
            const auto configuration =
                spec.configuration() ? spec.configuration()->as<HfaConfiguration>() : nullptr;
            const auto schema =
                spec.exports() ? spec.exports()->as<AlignApi::AlignSchema>() : nullptr;
            if (configuration == nullptr || schema == nullptr) {
                return srt::Error(otter::AnalysisError::Internal,
                                  "this declaration carries no hfa configuration");
            }

            auto session = otter::onnx::openSession(spec, configuration->model, "hfa model");
            if (!session) {
                return session.takeError();
            }
            return std::unique_ptr<srt::InferenceExecutive>(
                new HfaExecutive(spec, session.take(), *schema, *configuration));
        }

    private:
        /// Checks the exports against the model's files.
        ///
        /// A language that the exports declare but no dictionary covers, or a silence label absent
        /// from the vocabulary, would otherwise surface as a failed execution long after the
        /// package loaded.
        ///
        /// The audio format is a declaration of the same kind, and leaving it unchecked is more
        /// harmful: the host prepares the audio exactly as declared, so a sample rate at which
        /// these models were not trained does not cause a failure. The model then aligns at the
        /// wrong speed, and the result is plausible but wrong.
        ///
        /// \return An empty value, or \c srt::Error::InvalidFormat or \c srt::Error::FileNotFound
        ///         for the first mismatch.
        static srt::Expected<void> checkAgainstModel(const AlignApi::AlignSchema &declared,
                                                     const HfaConfiguration &wiring) {
            const auto &model = wiring.facts;
            if (declared.sampleRate != model.sampleRate) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the hfa variant runs at " + std::to_string(model.sampleRate) +
                                      " Hz; the exports declare " +
                                      std::to_string(declared.sampleRate));
            }
            std::set<std::string> seenLanguages;
            for (const auto &entry : declared.languages) {
                const auto &language = entry.language;
                // The variant has one model code, and therefore one dictionary, per language, and
                // provides no second notation of the same language under another scheme.
                if (!seenLanguages.insert(language).second) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the exports list " + language +
                                          " more than once, but the hfa variant has one dictionary "
                                          "per language");
                }
                const auto code = wiring.languages.find(language);
                if (code == wiring.languages.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the exports list the language " + language +
                                          " but the configuration gives it no model code");
                }
                const auto named = model.dictionaries.find(code->second);
                if (named == model.dictionaries.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the model's vocabulary has no dictionary for the language " +
                                          language + " (" + code->second + ")");
                }
                const auto path = wiring.vocab.parent_path() / named->second;
                if (!std::filesystem::is_regular_file(path)) {
                    return srt::Error(srt::Error::FileNotFound,
                                      "the dictionary " + named->second + " of " + language +
                                          " is not in the directory of the vocabulary");
                }
                // The declared phonemes must equal the phonemes that the classifier can emit for
                // this language: every vocabulary label with the model's code prefix, without the
                // prefix. A list with an extra or a missing phoneme would make a host evaluate
                // compatibility with a singer against the wrong set. This check also rejects a code
                // without any phoneme in the vocabulary, for which decoding would yield a span with
                // no words.
                const std::string prefix = code->second + LANGUAGE_SEPARATOR;
                std::set<std::string> emitted;
                for (const auto &[label, index] : model.vocabulary) {
                    if (label.rfind(prefix, 0) == 0) {
                        emitted.insert(label.substr(prefix.size()));
                    }
                }
                const std::set<std::string> promised(entry.phonemes.begin(), entry.phonemes.end());
                if (promised != emitted) {
                    std::string missing;
                    for (const auto &label : emitted) {
                        if (promised.count(label) == 0) {
                            missing += " " + label;
                        }
                    }
                    std::string extra;
                    for (const auto &label : promised) {
                        if (emitted.count(label) == 0) {
                            extra += " " + label;
                        }
                    }
                    return srt::Error(
                        srt::Error::InvalidFormat,
                        "the phonemes the exports list for " + language + " differ from the "
                        "phonemes in the model vocabulary for " + code->second +
                            (missing.empty() ? "" : "; not listed:" + missing) +
                            (extra.empty() ? "" : "; listed but unknown:" + extra));
                }
            }
            for (const auto &label : declared.nonSpeechPhonemes) {
                if (std::find(model.nonSpeechClasses.begin(), model.nonSpeechClasses.end(), label) ==
                    model.nonSpeechClasses.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the exports list the non-speech phoneme " + label +
                                          ", which the model's vocabulary does not list among its "
                                          "non-lexical phonemes");
                }
            }
            // The splitter also separates words with the silence label, and the decode treats it
            // as silence, so it must be a class that the model emits and one of the model's silent
            // classes. The reference hard-codes that class as SP at index 0; this port reads the
            // index from the vocabulary instead, so this check is the only remaining assumption.
            if (!isSilent(model.silentPhonemes, declared.silenceLabel)) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the exports name " + declared.silenceLabel +
                                      " as the silence label, which the model's vocabulary does "
                                      "not list among its silent phonemes");
            }
            if (model.vocabulary.find(declared.silenceLabel) == model.vocabulary.end()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the exports name " + declared.silenceLabel +
                                      " as the silence label, which is not a class of the model's "
                                      "vocabulary");
            }
            return srt::Expected<void>();
        }

        /// Reads the dictionary of every language the exports declare.
        static srt::Expected<void> loadDictionaries(const AlignApi::AlignSchema &declared,
                                                    HfaConfiguration &wiring) {
            for (const auto &entry : declared.languages) {
                // checkAgainstModel() has verified the code and the dictionary's name.
                const auto &code = wiring.languages.at(entry.language);
                const auto &name = wiring.facts.dictionaries.at(code);
                auto dictionary = otter::hfa::G2p::load(wiring.vocab.parent_path() / name);
                if (!dictionary) {
                    return dictionary.takeError().withContext("cannot read the " + entry.language +
                                                              " dictionary");
                }
                wiring.dictionaries.emplace(entry.language, dictionary.take());
            }
            return srt::Expected<void>();
        }

        static srt::Expected<std::unique_ptr<HfaConfiguration>>
            readConfiguration(const srt::ContribSpec &spec) {
            const auto &value = spec.manifestConfiguration();
            if (!value.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the hfa configuration must be an object");
            }
            const auto object = value.toObject();
            if (auto checked = otter::manifest::rejectUnknownKeys(
                    object,
                    {"model", "config", "vocab", "languages"},
                    "the hfa configuration");
                !checked) {
                return checked.takeError();
            }

            auto result = std::make_unique<HfaConfiguration>();
            if (auto read =
                    otter::onnx::readPaths(object, spec.declarationPath().parent_path(),
                                           {
                                               {"model",  &result->model },
                                               {"config", &result->config},
                                               {"vocab",  &result->vocab }
            },
                                           {"model", "config", "vocab"}, "the hfa configuration");
                !read) {
                return read.takeError();
            }

            const auto languages = object.find("languages");
            if (languages == object.end() || !languages->second.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the hfa configuration requires a languages object that maps the "
                                  "identifiers the exports list to the model's own codes");
            }
            for (const auto &[name, code] : languages->second.toObject()) {
                if (name.empty()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "a language identifier must not be empty");
                }
                auto text = otter::manifest::readString(code, "the code of " + name);
                if (!text) {
                    return text.takeError();
                }
                result->languages.emplace(name, text.take());
            }
            if (result->languages.empty()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the hfa configuration declares no language");
            }

            return result;
        }
    };

    class HfaPlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(std::string_view interfaceName, int level, std::string_view variant) override {
            if (auto served =
                    otter::onnx::checkServed(interfaceName, level, variant, AlignApi::API_INTERFACE,
                                             AlignApi::API_LEVEL, VARIANT);
                !served) {
                return served.takeError();
            }
            return std::unique_ptr<srt::ContribInterpreter>(new HfaInterpreter());
        }
    };

}

STDC_EXPORT_PLUGIN(HfaPlugin)
