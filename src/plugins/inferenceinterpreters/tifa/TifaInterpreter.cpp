#include "TifaInterpreter.h"

// The tifa forced alignment provider, ported from the TIFA singing-voice aligner's ONNX export.
// TIFA is told what is sung and answers with the phonemes it found and where in the audio each one
// sits. Five graphs split the work: the mel spectrogram, the candidate grid's template, the
// fragment scores, the candidate choice, and the tokens the choice kept, and what is left to the
// host is the decode: a flat Viterbi over the raw cosine similarities, and the words and timings
// the contract asks for. The text side runs the pipeline the exporter documents. Where this port
// deliberately differs from the exporter's own inference script is registered one line at a time in
// docs/plans/TifaAlign.md §4.5.

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
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

#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/Support/Expected.h>
#include <synthrt/Support/Json.h>

#include <dsinfer/Core/Tensor.h>
#include <dsinfer/Inference/InferenceSession.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/Provider/AnalysisInput.h>
#include <otter/Analysis/Provider/AnalysisInterpreter.h>
#include <otter/Api/Align/1/AlignApiL1.h>
#include <analysisutil/ManifestValues.h>

#include "Decode.h"
#include "Grid.h"
#include <onnxutil/OnnxSupport.h>
#include "Scoring.h"

namespace AlignApi = otter::Api::Align::L1;

namespace otter::tifa {

    namespace {

        // The language separator the model's symbols carry, as in <tt>zh/a</tt>.
        constexpr char LANGUAGE_SEPARATOR = '/';

        // The frame shift the export is built for, used when the model's own config.json does not
        // state one. It is the hop of the exported front end, 10 ms at 48 kHz, and a package
        // whose graphs were built for another shift says so in the file rather than here.
        constexpr double FALLBACK_TIMESTEP = 0.01;

        // The schemes of the writing systems the exporter's dictionaries cover, as wolf names
        // them.
        //
        // The scheme is checked rather than derived from the language, because one language can be
        // written two ways and the phonemes of one notation are not the phonemes of another. These
        // four are also what the writing systems of the singer side are named, which is what lets
        // a host pair this aligner with a singer of the same language.
        constexpr char SCHEMES[][9] = {"pinyin", "jyutping", "romaji", "arpabet"};

        // The languages a configuration can carry a dictionary for, and the key it carries it
        // under.
        //
        // The key names the language rather than the model's code for it: which file holds a
        // language's pronunciations is the package's business, and the model's code is a separate
        // mapping because the vocabulary prefixes its own symbols with it.
        constexpr std::pair<const char *, const char *> DICTIONARY_KEYS[] = {
            {"cmn", "dictionaryCmn"},
            {"yue", "dictionaryYue"},
            {"jpn", "dictionaryJpn"},
            {"eng", "dictionaryEng"},
        };

        using TensorPtr = std::shared_ptr<ds::ITensor>;

        // What the exporter's config.json says about the graphs.
        struct ModelFacts {
            // The rate the front end is built for. A declaration stating another one would have
            // the host prepare audio the model then misplaces.
            int sampleRate = 0;

            // Seconds between adjacent frames, which is what turns a frame index into a time.
            double timestep = FALLBACK_TIMESTEP;

            // The mel bins the spectrogram graph emits, when the file states them.
            std::optional<int> melBins;

            // The classes the model graph scores, when the file states them.
            std::optional<int> classes;
        };

        // Reads the exporter's config.json.
        srt::Expected<ModelFacts> readModelFacts(const std::filesystem::path &path) {
            auto document = otter::onnx::readJsonObject(path, "the model's config.json");
            if (!document) {
                return document.takeError();
            }
            const auto root = document.take().toObject();

            const auto rate = root.find("samplerate");
            if (rate == root.end()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the model's config.json states no samplerate");
            }
            auto sampleRate =
                otter::manifest::readPositiveInt(rate->second, "config.json samplerate");
            if (!sampleRate) {
                return sampleRate.takeError();
            }

            ModelFacts facts;
            facts.sampleRate = sampleRate.take();
            // The frame shift and the two widths are read when the file states them: they describe
            // the exported graph rather than the contract, so a file that leaves one out is not
            // refused, and what can be checked against the graph is checked at the first execution
            // instead.
            if (const auto it = root.find("timestep"); it != root.end()) {
                auto timestep =
                    otter::manifest::readPositiveDouble(it->second, "config.json timestep");
                if (!timestep) {
                    return timestep.takeError();
                }
                facts.timestep = timestep.take();
            }
            const std::pair<const char *, std::optional<int> *> widths[] = {
                {"num_mels",   &facts.melBins},
                {"vocab_size", &facts.classes},
            };
            for (const auto &[key, member] : widths) {
                const auto it = root.find(key);
                if (it == root.end()) {
                    continue;
                }
                auto value =
                    otter::manifest::readPositiveInt(it->second, std::string("config.json ") + key);
                if (!value) {
                    return value.takeError();
                }
                *member = value.take();
            }
            return facts;
        }

        // Whether \a scheme is one of the writing systems this variant reads.
        bool isKnownScheme(const std::string &scheme) {
            for (const auto &known : SCHEMES) {
                if (scheme == known) {
                    return true;
                }
            }
            return false;
        }

        // The dictionary key of \a language, or null when this variant covers no such language.
        //
        // A language outside the four the exporter ships dictionaries for is refused rather than
        // looked up by a derived name: a key the declaration could spell in a way this variant did
        // not expect is a dictionary it would silently not read.
        const char *dictionaryKey(const std::string &language) {
            for (const auto &[covered, key] : DICTIONARY_KEYS) {
                if (language == covered) {
                    return key;
                }
            }
            return nullptr;
        }

        // The symbols of the vocabulary that carry no language, which any language's result may
        // use.
        //
        // The exporter gives a symbol that two languages merge one number and one spelling as
        // well, so a symbol with no prefix means that no language in particular applies rather
        // than that no language exists.
        std::set<std::string> sharedSymbols(const otter::tifa::Vocabulary &vocabulary) {
            std::set<std::string> shared;
            for (const auto &[label, number] : vocabulary.symbols()) {
                if (label.find(LANGUAGE_SEPARATOR) == std::string::npos) {
                    shared.insert(label);
                }
            }
            return shared;
        }

        // The phonemes a language's symbols hold, without the prefix that names the language.
        std::set<std::string> emittedOf(const otter::tifa::Vocabulary &vocabulary,
                                        const std::string &code) {
            const std::string prefix = code + LANGUAGE_SEPARATOR;
            std::set<std::string> emitted;
            for (const auto &[label, number] : vocabulary.symbols()) {
                if (label.rfind(prefix, 0) == 0) {
                    emitted.insert(label.substr(prefix.size()));
                }
            }
            return emitted;
        }

        // Lists a difference set the way a reader can act on it.
        std::string spelled(const std::set<std::string> &labels) {
            std::string result;
            for (const auto &label : labels) {
                result += " " + label;
            }
            return result;
        }

        // What this variant reads from its configuration block: the five graphs, the files the
        // export came with, and the map from the host's language identifiers to the model's own
        // codes. The audio format, the languages and the phonemes are contract facts and live in
        // exports.
        class TifaConfiguration : public srt::ContribConfiguration {
        public:
            TifaConfiguration() : srt::ContribConfiguration(Triplet) {
            }

            std::filesystem::path spectrogram;
            std::filesystem::path model;
            std::filesystem::path prepare;
            std::filesystem::path score;
            std::filesystem::path select;

            // The exporter's config.json: the rate and the frame shift its front end is built for.
            std::filesystem::path config;

            // The exporter's vocabulary.json: the symbols the model can emit and their numbers.
            std::filesystem::path vocabulary;

            // One dictionary per language the declaration lists, keyed by the host's identifier.
            std::map<std::string, std::filesystem::path> dictionaries;

            // Maps the identifiers the exports list to the codes the model speaks.
            //
            // The contract speaks identifiers because a model's own codes are its own: two models
            // need not agree that <tt>zh</tt> is the same language, or spell it the same way.
            std::map<std::string, std::string> languages;
        };

        // Everything the model side needs, read once when an analyzer is created.
        struct ModelFiles {
            ModelFacts facts;
            otter::tifa::Vocabulary vocabulary;

            // The model's own code for each declared language.
            std::map<std::string, std::string> codes;

            // One dictionary per language the exports declare, keyed by the host's identifier.
            std::map<std::string, otter::tifa::Dictionary> dictionaries;

            // Reads the files the configuration points at. A language without a dictionary is
            // refused here rather than at the first execution that asks for it.
            static srt::Expected<ModelFiles> load(const TifaConfiguration &configuration) {
                auto facts = readModelFacts(configuration.config);
                if (!facts) {
                    return facts.takeError();
                }
                auto vocabulary = otter::tifa::Vocabulary::read(configuration.vocabulary);
                if (!vocabulary) {
                    return vocabulary.takeError();
                }

                ModelFiles result;
                result.facts = facts.take();
                result.vocabulary = vocabulary.take();
                result.codes = configuration.languages;
                for (const auto &[language, path] : configuration.dictionaries) {
                    auto dictionary = otter::tifa::Dictionary::load(path);
                    if (!dictionary) {
                        return dictionary.takeError().withContext("cannot read the " + language +
                                                                  " dictionary");
                    }
                    result.dictionaries.emplace(language, dictionary.take());
                }
                return result;
            }
        };

        // The sessions one analyzer opens, together and in one place.
        //
        // The three text graphs are opened on the CPU: they are lookup, integer and boolean work,
        // which the accelerator backends do not all carry, and moving a handful of small tensors
        // across the bus to run them is slower than leaving them where they are.
        struct OpenedModels {
            std::unique_ptr<ds::InferenceSession> spectrogram;
            std::unique_ptr<ds::InferenceSession> model;
            std::unique_ptr<ds::InferenceSession> prepare;
            std::unique_ptr<ds::InferenceSession> score;
            std::unique_ptr<ds::InferenceSession> select;
        };

        // The same five graphs as the executive holds them, which is as handles: the executive
        // owns the sessions and its body runs them through these.
        struct Models {
            ds::InferenceSession *spectrogram = nullptr;
            ds::InferenceSession *model = nullptr;
            ds::InferenceSession *prepare = nullptr;
            ds::InferenceSession *score = nullptr;
            ds::InferenceSession *select = nullptr;
        };

        srt::Expected<TensorPtr> int64s(const std::vector<std::int64_t> &shape,
                                        const std::vector<std::int64_t> &values) {
            auto tensor = ds::Tensor::createFromView<std::int64_t>(
                shape, stdc::array_view<std::int64_t>{values});
            if (!tensor) {
                return tensor.takeError();
            }
            return TensorPtr(tensor.take());
        }

        srt::Expected<TensorPtr> floats(const std::vector<std::int64_t> &shape,
                                        const std::vector<float> &values) {
            auto tensor = ds::Tensor::createFromView<float>(shape, stdc::array_view<float>{values});
            if (!tensor) {
                return tensor.takeError();
            }
            return TensorPtr(tensor.take());
        }

        // A boolean tensor, which is one byte an element: what the graphs that take a mask or a
        // flag read whichever backend runs them.
        srt::Expected<TensorPtr> flags(const std::vector<std::int64_t> &shape,
                                       const std::vector<std::uint8_t> &values) {
            const stdc::array_view<std::byte> raw(
                reinterpret_cast<const std::byte *>(values.data()), values.size());
            auto tensor = ds::Tensor::createFromRawView(ds::ITensor::Bool, shape, raw);
            if (!tensor) {
                return tensor.takeError();
            }
            return TensorPtr(tensor.take());
        }

        // Lays a grid's per-row arrays out the way the graphs read them: one row after another.
        std::vector<std::int64_t> flatten(const std::vector<std::vector<std::int64_t>> &perRow) {
            std::vector<std::int64_t> flat;
            for (const auto &row : perRow) {
                flat.insert(flat.end(), row.begin(), row.end());
            }
            return flat;
        }

        // Reads a tensor of whole numbers, as one row of everything it holds.
        srt::Expected<std::vector<std::int64_t>> readInt64s(const TensorPtr &tensor,
                                                            std::size_t count, const char *what) {
            if (!tensor) {
                return srt::Error(otter::AnalysisError::ModelFailed,
                                  std::string(what) + " is missing");
            }
            if (tensor->dataType() != ds::ITensor::Int64) {
                return srt::Error(otter::AnalysisError::ModelMismatch,
                                  std::string(what) + " is not a whole-number tensor");
            }
            const auto view = tensor->view<std::int64_t>();
            if (view.size() != count) {
                return srt::Error(otter::AnalysisError::ModelMismatch,
                                  std::string(what) + " holds " + std::to_string(view.size()) +
                                      " values, not " + std::to_string(count));
            }
            return std::vector<std::int64_t>(view.begin(), view.end());
        }

        // Reads a boolean tensor as one flag per element.
        srt::Expected<std::vector<std::uint8_t>> readFlags(const TensorPtr &tensor,
                                                           std::size_t count, const char *what) {
            if (!tensor) {
                return srt::Error(otter::AnalysisError::ModelFailed,
                                  std::string(what) + " is missing");
            }
            if (tensor->dataType() != ds::ITensor::Bool) {
                return srt::Error(otter::AnalysisError::ModelMismatch,
                                  std::string(what) + " is not a boolean tensor");
            }
            const auto raw = tensor->rawView();
            if (raw.size() != count) {
                return srt::Error(otter::AnalysisError::ModelMismatch,
                                  std::string(what) + " holds " + std::to_string(raw.size()) +
                                      " values, not " + std::to_string(count));
            }
            std::vector<std::uint8_t> result(raw.size());
            for (std::size_t i = 0; i < raw.size(); ++i) {
                result[i] = raw[i] == std::byte{0} ? 0 : 1;
            }
            return result;
        }

        // Reads a tensor of scores, as one row of everything it holds.
        srt::Expected<std::vector<float>> readFloats(const TensorPtr &tensor, std::size_t count,
                                                     const char *what) {
            if (!tensor) {
                return srt::Error(otter::AnalysisError::ModelFailed,
                                  std::string(what) + " is missing");
            }
            if (tensor->dataType() != ds::ITensor::Float) {
                return srt::Error(otter::AnalysisError::ModelMismatch,
                                  std::string(what) + " is not a tensor of scores");
            }
            const auto view = tensor->view<float>();
            if (view.size() != count) {
                return srt::Error(otter::AnalysisError::ModelMismatch,
                                  std::string(what) + " holds " + std::to_string(view.size()) +
                                      " values, not " + std::to_string(count));
            }
            return std::vector<float>(view.begin(), view.end());
        }

        // Checks that one output is a tensor of exactly the shape a graph promised.
        //
        // The text graphs describe a grid the host handed over, and the two meet in these shapes:
        // a tensor of another rank or of another capacity is one whose values cannot be read at
        // the offsets the grid is in, so it is refused where it arrives rather than indexed into.
        srt::Expected<std::vector<std::int64_t>> expectShape(const TensorPtr &tensor,
                                                             const std::vector<std::int64_t> &shape,
                                                             const char *what) {
            if (!tensor) {
                return srt::Error(otter::AnalysisError::ModelFailed,
                                  std::string("the graph returned no ") + what);
            }
            if (tensor->shape() != shape) {
                return srt::Error(otter::AnalysisError::ModelMismatch,
                                  std::string(what) +
                                      " is not a tensor of the shape the grid it was "
                                      "asked about calls for");
            }
            return shape;
        }

        class TifaExecutive : public otter::onnx::OnnxExecutive<AlignApi::Executive> {
        public:
            TifaExecutive(srt::InferenceSpec &spec, OpenedModels opened,
                          const AlignApi::Exports &schema, ModelFiles files)
                : OnnxExecutive(spec), m_schema(schema), m_files(std::move(files)) {
                m_models.spectrogram = own(std::move(opened.spectrogram));
                m_models.model = own(std::move(opened.model));
                m_models.prepare = own(std::move(opened.prepare));
                m_models.score = own(std::move(opened.score));
                m_models.select = own(std::move(opened.select));
            }

            ~TifaExecutive() override {
                // The sessions are owned by this object, so an execution still in flight has to be
                // stopped and waited out before they are closed: the body reads them.
                (void) quit();
                (void) wait();
                closeSessions();
            }

        protected:
            srt::Expected<std::unique_ptr<AlignApi::Result>>
                run(const AlignApi::StartInput &input) override {
                const auto &audio = input.audio;
                auto prepared = otter::PreparedSamples::prepare(
                    audio, m_schema.sampleRate, m_schema.channelCount, m_schema.maxSegmentDuration);
                if (!prepared) {
                    return prepared.takeError();
                }
                const auto waveform = prepared.take();
                if (waveform.empty()) {
                    return srt::Error(srt::Error::InvalidArgument, "the span carries no audio");
                }

                // An aligner that is not told what to look for has nothing to align: answering with
                // a transcription instead would be a different contract.
                if (input.lyrics.empty()) {
                    return srt::Error(
                        srt::Error::InvalidArgument,
                        "an alignment needs to know what is sung, and lyrics is empty");
                }

                const std::string language = input.language.value_or(m_schema.defaultLanguage);
                if (language.empty()) {
                    return srt::Error(
                        srt::Error::InvalidArgument,
                        "this package declares no default language, so the caller has to "
                        "name one");
                }
                // The scheme specifies the notation of the result's phonemes. A language may be
                // declared with several schemes, in which case the caller must specify the scheme.
                const AlignApi::LanguageInfo *entry = nullptr;
                for (const auto &candidate : m_schema.languages) {
                    if (candidate.language != language ||
                        (input.scheme && candidate.scheme != *input.scheme)) {
                        continue;
                    }
                    if (entry != nullptr) {
                        return srt::Error(
                            srt::Error::InvalidArgument,
                            "this package declares " + language +
                                " with more than one scheme, so the caller must specify "
                                "a scheme");
                    }
                    entry = &candidate;
                }
                if (entry == nullptr) {
                    return srt::Error(
                        srt::Error::InvalidArgument,
                        "the caller asked for the language " + language +
                            (input.scheme ? " with the scheme " + *input.scheme : "") +
                            ", which this model cannot align");
                }
                const auto code = m_files.codes.find(language);
                const auto dictionary = m_files.dictionaries.find(language);
                if (code == m_files.codes.end() || dictionary == m_files.dictionaries.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "this package carries no dictionary for " + language);
                }

                // The model detects no non-speech sound of its own, so there is nothing to switch
                // on and nothing a request for one could mean. A caller asking for a label would
                // otherwise be told that none was found, which it cannot tell from there being
                // none.
                if (!input.nonSpeechPhonemes.empty()) {
                    return srt::Error(srt::Error::InvalidArgument,
                                      "the caller asked for the non-speech phoneme " +
                                          input.nonSpeechPhonemes.front() +
                                          ", which this model cannot report");
                }

                const auto report = [this, &input](double value) {
                    if (input.progress && !input.progress(value)) {
                        // The callback answered that the execution does not continue, so the stop
                        // is requested here and the calls to checkCancelled() below report it.
                        (void) stop();
                    }
                };
                report(0.0);
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }

                // 1. What the dictionary makes of the lyrics.
                auto grid = otter::tifa::buildGrid(input.lyrics, code->second, dictionary->second,
                                                   m_files.vocabulary);
                if (!grid) {
                    return grid.takeError();
                }
                if (grid->paths.empty()) {
                    return srt::Error(srt::Error::InvalidArgument,
                                      "none of the lyrics is in this language's dictionary");
                }

                // The length is computed the way the exporter computes it, one float division of
                // the sample count by the rate, because the graph masks its frames from the value
                // handed to it, and the timeline built from those frames has to come out the same
                // length.
                const double length = static_cast<double>(static_cast<float>(waveform.size()) /
                                                          static_cast<float>(m_schema.sampleRate));

                // 2. The spectrogram, over the whole span.
                auto features = runSpectrogram(waveform, length);
                if (!features) {
                    return features.takeError();
                }
                // By reference: the frames the mask kept are what the model is indexed by and what
                // the decode is measured on, and both read them off the features rather than a copy
                // that would have to be kept in step with them.
                const auto &heard = features->heard;
                if (heard.empty()) {
                    return srt::Error(
                        otter::AnalysisError::ModelMismatch,
                        "the spectrogram graph marked no frame of this span as audio");
                }
                report(0.3);
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }

                // 3. The text side: the template the score graph numbers, the reading the host's
                //    whole-word program picks for every word, and the tokens the decode runs on.
                auto read = prepareTemplate(*grid);
                if (!read) {
                    return read.takeError();
                }
                auto choices = chooseReadings(*grid, *read, *features);
                if (!choices) {
                    return choices.takeError();
                }
                report(0.5);
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }
                auto chosen = chooseTokens(*grid, choices.take());
                if (!chosen) {
                    return chosen.takeError();
                }

                // 4. What the model makes of the tokens that were chosen, which is what the decode
                //    reads.
                auto similarities = runSimilarities(*grid, *chosen, *features);
                if (!similarities) {
                    return similarities.takeError();
                }
                report(0.6);
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }

                // 5. The decode, over the frames the graph said carry audio.
                std::vector<std::int64_t> groups;
                std::vector<std::size_t> rows;
                for (std::size_t row = 0; row < chosen->valid.size(); ++row) {
                    if (chosen->valid[row] != 0) {
                        rows.push_back(row);
                        // The group is the one the select graph put the row in. The graph removes
                        // the rows no chosen reading fills before it numbers the groups, so a group
                        // is a run of the rows it kept, where the grid's own numbers would have
                        // counted rows that are no longer there.
                        groups.push_back(chosen->groups[row]);
                    }
                }
                const auto spans = otter::tifa::decodeFrames(similarities.take(), heard.size(),
                                                             rows.size(), groups);
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }
                report(0.85);

                // 6. Onto the words the caller wrote, and onto the host's timeline.
                auto words = place(*grid, entry->language, *chosen, rows, spans, heard, length,
                                   audio.startTime);
                if (!words) {
                    return words.takeError();
                }

                auto result = std::make_unique<AlignApi::Result>();
                result->language = entry->language;
                result->scheme = entry->scheme;
                result->words = words.take();
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }
                report(1.0);
                return result;
            }

        private:
            // The spectrogram of one span, and the frames of it that carry audio.
            struct Features {
                TensorPtr spectrogram;
                TensorPtr maskT;

                // The frame indices the mask keeps, in order.
                std::vector<std::size_t> heard;
            };

            srt::Expected<Features> runSpectrogram(const otter::PreparedSamples &waveform,
                                                   double length) const {
                if (!m_models.spectrogram) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "this package carries no spectrogram graph");
                }
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }

                std::map<std::string, TensorPtr> inputs;
                {
                    auto tensor = otter::onnx::waveformTensor(waveform);
                    if (!tensor) {
                        return tensor.takeError();
                    }
                    inputs["waveform"] = tensor.take();
                }
                {
                    const std::vector<float> value = {static_cast<float>(length)};
                    auto tensor = floats({1}, value);
                    if (!tensor) {
                        return tensor.takeError();
                    }
                    inputs["duration"] = tensor.take();
                }
                auto produced = otter::onnx::run(*m_models.spectrogram, std::move(inputs),
                                                 {"spectrogram", "maskT"}, "spectrogram graph",
                                                 [this] { return cancelled(); });
                if (!produced) {
                    return produced.takeError();
                }
                const auto outputs = produced.take();

                Features features;
                features.spectrogram = outputs.at("spectrogram");
                features.maskT = outputs.at("maskT");
                const auto shape = features.spectrogram->shape();
                if (shape.size() != 3 || shape[0] != 1 || shape[1] <= 0 || shape[2] <= 0) {
                    return srt::Error(otter::AnalysisError::ModelMismatch,
                                      "the spectrogram graph returned a tensor that is not "
                                      "[1, frames, mel bins]");
                }
                const auto frames = static_cast<std::size_t>(shape[1]);
                if (m_files.facts.melBins && shape[2] != *m_files.facts.melBins) {
                    return srt::Error(otter::AnalysisError::ModelMismatch,
                                      "the spectrogram graph emits " + std::to_string(shape[2]) +
                                          " mel bins, and the model's config.json states " +
                                          std::to_string(*m_files.facts.melBins));
                }
                auto mask = readFlags(features.maskT, frames, "maskT");
                if (!mask) {
                    return mask.takeError();
                }
                const auto masked = mask.take();
                for (std::size_t frame = 0; frame < masked.size(); ++frame) {
                    if (masked[frame] != 0) {
                        features.heard.push_back(frame);
                    }
                }
                return features;
            }

            // The template a grid is scored through: the token the model is asked about in every
            // row before any reading has been picked, and the rows and segments the fragments are
            // made of.
            struct Template {
                // The tokens the model is asked about, one per grid row: the exporter's own
                // template, which reads a slot a reading could move as its MASK rather than as the
                // first reading.
                TensorPtr tokens;

                // Which rows of that template carry a token at all.
                TensorPtr maskN;

                std::vector<std::int64_t> values;
                std::vector<std::int64_t> segments;
                std::vector<std::int64_t> mapping;

                // Whether any row is a slot a reading could move, which is all there is to choose
                // between: with none of them every reading of every word is the same phrase.
                bool scored = false;
            };

            // The template one grid makes, as the prepare graph reads it.
            srt::Expected<Template> prepareTemplate(const otter::tifa::Grid &grid) const {
                if (!m_models.prepare) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "this package carries no prepare graph");
                }
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }

                const auto rows = static_cast<std::int64_t>(grid.paths.size());
                const auto columns = static_cast<std::int64_t>(grid.columns());
                std::map<std::string, TensorPtr> inputs;
                {
                    auto paths = int64s({1, rows, columns}, flatten(grid.paths));
                    if (!paths) {
                        return paths.takeError();
                    }
                    inputs["paths"] = paths.take();
                    auto words = int64s({1, rows}, grid.words);
                    if (!words) {
                        return words.takeError();
                    }
                    inputs["words"] = words.take();

                    // The candidates the graph is told about are the grid's own, prefix packed:
                    // which columns a word has a reading in is what tells an empty reading from an
                    // absent one.
                    std::vector<std::uint8_t> packed;
                    packed.reserve(grid.candidates.size() * grid.columns());
                    for (const auto &word : grid.candidates) {
                        for (const auto flag : word) {
                            packed.push_back(flag ? 1 : 0);
                        }
                    }
                    auto candidates = flags(
                        {1, static_cast<std::int64_t>(grid.candidates.size()), columns}, packed);
                    if (!candidates) {
                        return candidates.takeError();
                    }
                    inputs["candidates"] = candidates.take();

                    // The scoring unit is the reference's default one: a slot is masked where the
                    // readings of a word disagree about it, which is what a false grouped means.
                    // The other unit masks a whole word as soon as any slot of it disagrees, and
                    // would only ever score fewer rows than the grid it was handed.
                    auto grouped = flags({}, {0});
                    if (!grouped) {
                        return grouped.takeError();
                    }
                    inputs["grouped"] = grouped.take();
                }

                auto produced = otter::onnx::run(*m_models.prepare, std::move(inputs),
                                                 {"tokens", "segments", "mapping"}, "prepare graph",
                                                 [this] { return cancelled(); });
                if (!produced) {
                    return produced.takeError();
                }
                const auto outputs = produced.take();

                Template view;
                view.tokens = outputs.at("tokens");
                auto values = readInt64s(view.tokens, grid.paths.size(), "tokens");
                if (!values) {
                    return values.takeError();
                }
                view.values = values.take();
                auto segments = readInt64s(outputs.at("segments"), grid.paths.size(), "segments");
                if (!segments) {
                    return segments.takeError();
                }
                view.segments = segments.take();
                auto mapping = readInt64s(outputs.at("mapping"), grid.paths.size(), "mapping");
                if (!mapping) {
                    return mapping.takeError();
                }
                view.mapping = mapping.take();

                std::vector<std::uint8_t> mask(view.values.size());
                for (std::size_t row = 0; row < view.values.size(); ++row) {
                    mask[row] = view.values[row] != 0 ? 1 : 0;
                }
                auto maskN = flags({1, rows}, mask);
                if (!maskN) {
                    return maskN.takeError();
                }
                view.maskN = maskN.take();
                view.scored = std::any_of(view.segments.begin(), view.segments.end(),
                                          [](std::int64_t segment) { return segment > 0; });
                return view;
            }

            // The fragment table the score graph reported, checked against the grid it was asked
            // about: every axis of it is a capacity the grid's own shape decides.
            srt::Expected<otter::tifa::Fragments>
                readFragments(const otter::tifa::Grid &grid,
                              const std::map<std::string, TensorPtr> &outputs) const {
                const auto &descriptors = outputs.at("descriptors");
                const auto descriptorShape = descriptors->shape();
                if (descriptorShape.size() != 3 || descriptorShape[0] != 1 ||
                    descriptorShape[2] != 2) {
                    return srt::Error(otter::AnalysisError::ModelMismatch,
                                      "descriptors is not one word and one segment per fragment");
                }
                const auto fragments = static_cast<std::int64_t>(descriptorShape[1]);
                const auto columns = static_cast<std::int64_t>(grid.columns());

                otter::tifa::Fragments table;
                table.fragments = static_cast<std::size_t>(fragments);
                table.candidates = static_cast<std::size_t>(columns);
                {
                    auto values = readInt64s(descriptors, table.fragments * 2, "descriptors");
                    if (!values) {
                        return values.takeError();
                    }
                    const auto flat = values.take();
                    table.descriptors.reserve(table.fragments);
                    for (std::size_t fragment = 0; fragment < table.fragments; ++fragment) {
                        table.descriptors.push_back({flat[fragment * 2], flat[fragment * 2 + 1]});
                    }
                }
                const std::pair<const char *, std::vector<std::int64_t>> counts[] = {
                    {"lengths", {1, fragments, columns}           },
                    {"costs",   {1, fragments, columns, fragments}},
                    {"tails",   {1, fragments, fragments}         },
                };
                for (const auto &[name, shape] : counts) {
                    if (auto checked = expectShape(outputs.at(name), shape, name); !checked) {
                        return checked.takeError();
                    }
                }
                {
                    auto lengths = readInt64s(outputs.at("lengths"),
                                              table.fragments * table.candidates, "lengths");
                    if (!lengths) {
                        return lengths.takeError();
                    }
                    table.lengths = lengths.take();
                    auto costs =
                        readFloats(outputs.at("costs"),
                                   table.fragments * table.candidates * table.fragments, "costs");
                    if (!costs) {
                        return costs.takeError();
                    }
                    table.costs = costs.take();
                    auto tails =
                        readFloats(outputs.at("tails"), table.fragments * table.fragments, "tails");
                    if (!tails) {
                        return tails.takeError();
                    }
                    table.tails = tails.take();
                }
                {
                    // The capacity row is the one axis the graph numbers itself: the exporter
                    // writes one entry per fragment, a graph that leaves the padding entry out
                    // writes one per segment, and how many slots a segment has is read back out of
                    // whichever it wrote.
                    auto capacity =
                        readInt64s(outputs.at("capacity"),
                                   static_cast<std::size_t>(outputs.at("capacity")->shape().back()),
                                   "capacity");
                    if (!capacity) {
                        return capacity.takeError();
                    }
                    table.capacity = capacity.take();
                }
                return table;
            }

            // The reading the host picks for every word, numbered from one, zero for a word nothing
            // was read for.
            srt::Expected<std::vector<std::int64_t>>
                chooseReadings(const otter::tifa::Grid &grid, const Template &view,
                               const Features &features) const {
                // Every word keeps the first reading it has, which is what the reference does with
                // a sample the score graph found nothing to choose in: where no slot is one a
                // reading could move, there is no table to score and the choice is not one.
                std::vector<std::int64_t> choices;
                choices.reserve(grid.candidates.size());
                for (const auto &candidate : grid.candidates) {
                    const auto first = std::find(candidate.begin(), candidate.end(), true);
                    choices.push_back(
                        first == candidate.end()
                            ? 0
                            : static_cast<std::int64_t>(std::distance(candidate.begin(), first)) +
                                  1);
                }
                if (!view.scored) {
                    return choices;
                }
                if (!m_models.score) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "this package carries no score graph");
                }

                auto logits = runModel(features, view.tokens, view.maskN, {"logits"});
                if (!logits) {
                    return logits.takeError();
                }
                const auto outputs = logits.take();

                const auto rows = static_cast<std::int64_t>(grid.paths.size());
                const auto columns = static_cast<std::int64_t>(grid.columns());
                std::map<std::string, TensorPtr> inputs;
                inputs["logits"] = outputs.at("logits");
                {
                    auto paths = int64s({1, rows, columns}, flatten(grid.paths));
                    if (!paths) {
                        return paths.takeError();
                    }
                    inputs["paths"] = paths.take();
                    auto words = int64s({1, rows}, grid.words);
                    if (!words) {
                        return words.takeError();
                    }
                    inputs["words"] = words.take();
                    auto segments = int64s({1, rows}, view.segments);
                    if (!segments) {
                        return segments.takeError();
                    }
                    inputs["segments"] = segments.take();
                    auto mapping = int64s({1, rows}, view.mapping);
                    if (!mapping) {
                        return mapping.takeError();
                    }
                    inputs["mapping"] = mapping.take();
                }

                auto scored =
                    otter::onnx::run(*m_models.score, std::move(inputs),
                                     {"descriptors", "lengths", "costs", "tails", "capacity"},
                                     "score graph", [this] { return cancelled(); });
                if (!scored) {
                    return scored.takeError();
                }
                auto table = readFragments(grid, scored.take());
                if (!table) {
                    return table.takeError();
                }
                auto picked = otter::tifa::chooseCandidates(table.take(), grid.candidates);
                if (!picked) {
                    return picked.takeError();
                }
                return picked.take();
            }

            // The tokens the decode runs on: one per row of the grid, chosen by the graph that
            // picks between a word's readings, with the rows it left empty marked out.
            struct ChosenTokens {
                TensorPtr tensor;

                // The rows the graph chose a token for, masked out of the score where it did not.
                TensorPtr mask;

                // The chosen token number of each row, 0 where the row holds no token.
                std::vector<std::int64_t> values;

                std::vector<std::uint8_t> valid;

                // The word and the group the select graph reports for each row of its own output.
                //
                // They are read from the graph rather than taken from the grid row of the same
                // number because that graph compacts: it removes the rows a chosen reading leaves
                // empty, so from the first such row on, row p of its output is a later row of the
                // grid. Reading the grid at p instead would hand one word the rows of the next,
                // and the label of a row that reading left empty, which is no label at all.
                std::vector<std::int64_t> words;
                std::vector<std::int64_t> groups;

                // The reading each word of the grid was read as, as a column of the grid, numbered
                // from zero.
                std::vector<std::size_t> candidates;
            };

            srt::Expected<ChosenTokens>
                chooseTokens(const otter::tifa::Grid &grid,
                             const std::vector<std::int64_t> &choices) const {
                if (!m_models.select) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "this package carries no select graph");
                }
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }
                if (choices.size() != grid.candidates.size()) {
                    return srt::Error(otter::AnalysisError::ModelMismatch,
                                      "a reading of a word the grid does not carry was chosen");
                }

                const auto rows = static_cast<std::int64_t>(grid.paths.size());
                const auto columns = static_cast<std::int64_t>(grid.columns());
                std::map<std::string, TensorPtr> inputs;
                {
                    // The grid is handed over in the layout the graph reads it in: one row per slot
                    // a reading can hold a phoneme in, one column per reading.
                    auto paths = int64s({1, rows, columns}, flatten(grid.paths));
                    if (!paths) {
                        return paths.takeError();
                    }
                    inputs["paths"] = paths.take();
                    auto words = int64s({1, rows}, grid.words);
                    if (!words) {
                        return words.takeError();
                    }
                    inputs["words"] = words.take();
                    auto groups = int64s({1, rows, columns}, flatten(grid.groups));
                    if (!groups) {
                        return groups.takeError();
                    }
                    inputs["groups"] = groups.take();

                    // The graph numbers candidate columns from one and reads zero as a word that
                    // was not read at all, which is what the host's whole-word program reported: a
                    // word that is not in the grid has no column, so every choice is a column the
                    // grid holds.
                    auto tensor = int64s({1, static_cast<std::int64_t>(choices.size())}, choices);
                    if (!tensor) {
                        return tensor.takeError();
                    }
                    inputs["choices"] = tensor.take();
                }

                auto produced =
                    otter::onnx::run(*m_models.select, std::move(inputs),
                                     {"best_tokens", "maskN", "best_words", "best_groups"},
                                     "select graph", [this] { return cancelled(); });
                if (!produced) {
                    return produced.takeError();
                }
                const auto outputs = produced.take();

                ChosenTokens chosen;
                chosen.tensor = outputs.at("best_tokens");
                auto values = readInt64s(chosen.tensor, grid.paths.size(), "best_tokens");
                if (!values) {
                    return values.takeError();
                }
                chosen.values = values.take();
                chosen.mask = outputs.at("maskN");
                auto valid = readFlags(chosen.mask, grid.paths.size(), "maskN");
                if (!valid) {
                    return valid.takeError();
                }
                chosen.valid = valid.take();
                auto words = readInt64s(outputs.at("best_words"), grid.paths.size(), "best_words");
                if (!words) {
                    return words.takeError();
                }
                chosen.words = words.take();
                auto groups =
                    readInt64s(outputs.at("best_groups"), grid.paths.size(), "best_groups");
                if (!groups) {
                    return groups.takeError();
                }
                chosen.groups = groups.take();

                chosen.candidates.assign(choices.size(), 0);
                for (std::size_t word = 0; word < choices.size(); ++word) {
                    // The graph numbers candidate columns from one and reads zero as a word that
                    // was not read at all, which is what the host's whole-word program reported: a
                    // word that is not in the grid has no column, so every choice is a column the
                    // grid holds.
                    if (choices[word] < 0 || choices[word] > columns) {
                        return srt::Error(otter::AnalysisError::ModelMismatch,
                                          "a reading of a word the grid does not carry was chosen");
                    }
                    if (choices[word] > 0) {
                        chosen.candidates[word] = static_cast<std::size_t>(choices[word]) - 1;
                    }
                }

                for (std::size_t row = 0; row < chosen.valid.size(); ++row) {
                    // A row the graph left empty carries the exporter's padding, which is not a
                    // phoneme: the model input stays rectangular, so the row stays too and is
                    // masked out of the score instead. A row it kept belongs to a word, and a row
                    // of no word is a row the graph should not have kept at all.
                    if (chosen.values[row] == 0) {
                        chosen.valid[row] = 0;
                    }
                }
                return chosen;
            }

            // Runs the model graph on \a tokens over the frames \a features holds and hands back
            // the outputs it was asked for.
            //
            // The model is run twice in a scored phrase and once in any other, on two different
            // token sequences: the first time on the template, for the logits the candidates are
            // compared through, and then on the tokens the winning reading spells, which is what
            // the decode places.
            srt::Expected<std::map<std::string, TensorPtr>>
                runModel(const Features &features, const TensorPtr &tokens, const TensorPtr &maskN,
                         const std::set<std::string> &wanted) const {
                if (!m_models.model) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "this package carries no model graph");
                }
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }

                std::map<std::string, TensorPtr> inputs;
                inputs["spectrogram"] = features.spectrogram;
                inputs["tokens"] = tokens;
                inputs["maskT"] = features.maskT;
                inputs["maskN"] = maskN;

                auto produced = otter::onnx::run(*m_models.model, std::move(inputs), wanted,
                                                 "model graph", [this] { return cancelled(); });
                if (!produced) {
                    return produced.takeError();
                }
                return produced.take();
            }

            // The similarities the decode runs on: one row per frame the mask keeps, one column per
            // token the reading spells.
            srt::Expected<std::vector<float>> runSimilarities(const otter::tifa::Grid &grid,
                                                              const ChosenTokens &tokens,
                                                              const Features &features) const {
                auto produced = runModel(features, tokens.tensor, tokens.mask, {"similarities"});
                if (!produced) {
                    return produced.takeError();
                }
                const auto outputs = produced.take();

                // The score is read as [frames, tokens] and is the only place the two graphs'
                // shapes meet, so it is the one place where a mismatch between them is caught
                // rather than indexed past.
                const auto tensor = outputs.at("similarities");
                const auto shape = tensor->shape();
                const auto tokensInGrid = static_cast<std::int64_t>(grid.paths.size());
                if (shape.size() != 3 || shape[0] != 1 || shape[2] != tokensInGrid) {
                    return srt::Error(otter::AnalysisError::ModelMismatch,
                                      "the model graph returned a tensor that is not "
                                      "[1, frames, tokens]");
                }
                if (shape[1] <= 0 || static_cast<std::size_t>(shape[1]) <= features.heard.back()) {
                    return srt::Error(otter::AnalysisError::ModelMismatch,
                                      "the model graph scored " + std::to_string(shape[1]) +
                                          " frames, and the spectrogram graph produced " +
                                          std::to_string(features.heard.back() + 1) + " or more");
                }

                // The decode wants the tokens it can place, in order: a column whose row the graph
                // left empty is not a token and must not take a frame.
                const auto view = tensor->view<float>();
                std::vector<float> result;
                result.reserve(features.heard.size() * tokens.valid.size());
                for (const auto frame : features.heard) {
                    for (std::size_t row = 0; row < tokens.valid.size(); ++row) {
                        if (tokens.valid[row] != 0) {
                            result.push_back(view[frame * tokens.valid.size() + row]);
                        }
                    }
                }
                return result;
            }

            // Turns the decoded frame runs into the words the contract asks for.
            //
            // The decode places tokens, not words: one word may be several phonemes, and the frames
            // it did not take belong to no word at all. So the phones are gathered back onto the
            // word they were written for, and every stretch between two words, the ends of the span
            // included, becomes a word of its own carrying the declaration's silence label. That is
            // what makes the result cover the span: a host that wants only what was sung filters on
            // that label rather than reconstructing the gaps.
            //
            // \a language is the caller's identifier for the language in use, which is what the
            // package maps to the model's own code: the code whose prefix the phonemes of that
            // language are spelled without when a result reports them.
            srt::Expected<std::vector<AlignApi::WordInfo>>
                place(const otter::tifa::Grid &grid, const std::string &language,
                      const ChosenTokens &chosen, const std::vector<std::size_t> &rows,
                      const std::vector<otter::tifa::Span> &spans,
                      const std::vector<std::size_t> &heard, double length, double anchor) const {
                const auto seconds = [this](std::size_t frame) {
                    return static_cast<double>(frame) * m_files.facts.timestep;
                };
                // A span is half open, so a token that took the frames up to b ends where the frame
                // after b begins.
                const auto frameEnd = [&heard](const otter::tifa::Span &span) {
                    return heard[span.end - 1] + 1;
                };

                // One entry per grid row the decode placed, in phrase order: the row, and the
                // frames it took. A row the decode left empty is a phoneme with no frame, which the
                // reference's zero-width handling drops, so it is dropped here rather than reported
                // as a phone of no length, which the contract has no way to express.
                struct Placed {
                    // The word of the grid the row is a phone of, numbered from zero.
                    std::size_t word = 0;

                    // What the reading that word was read as spells at that phone, as the result
                    // reports it. Never empty: a reading whose phoneme the vocabulary cannot spell
                    // is no reading at all, so it was never handed to the model.
                    std::string label;

                    std::size_t first = 0;
                    std::size_t last = 0;
                };
                std::vector<Placed> placed;
                // Each row the select graph kept is the next phone of the reading its word was read
                // as, in reading order: the graph compacts rows rather than renumbering them, so
                // the order is what says which phoneme a row is, and a word's own count of them is
                // what says which phoneme of its reading comes next. A row the decode gave no frame
                // is still a phoneme of the reading and still takes its place in that count.
                std::vector<std::size_t> nextPhone(grid.wordCount(), 0);
                for (std::size_t index = 0; index < rows.size(); ++index) {
                    const auto word = chosen.words[rows[index]];
                    if (word < 1 || word > static_cast<std::int64_t>(grid.wordCount())) {
                        return srt::Error(
                            otter::AnalysisError::ModelMismatch,
                            "the select graph placed a phone of no word of the phrase");
                    }
                    const auto number = static_cast<std::size_t>(word) - 1;
                    const auto &reading = grid.readings[number][chosen.candidates[number]];
                    auto &next = nextPhone[number];
                    if (next >= reading.size()) {
                        return srt::Error(otter::AnalysisError::ModelMismatch,
                                          "the select graph placed more phones for " +
                                              grid.texts[number] +
                                              " than the reading it chose spells");
                    }
                    const auto label = labelOf(reading[next], language);
                    ++next;
                    if (label.empty()) {
                        // Reported rather than kept: a phone of no label is one the caller cannot
                        // place anywhere, and the declaration promised this language's phonemes,
                        // not gaps.
                        return srt::Error(srt::Error::InvalidFormat,
                                          "the vocabulary spells a phoneme of " +
                                              grid.texts[number] + " with nothing");
                    }
                    if (spans[index].start >= spans[index].end) {
                        continue;
                    }
                    placed.push_back(
                        {number, label, heard[spans[index].start], frameEnd(spans[index])});
                }

                std::vector<AlignApi::WordInfo> words;
                const auto silence = [this, &words](double from, double to) {
                    AlignApi::WordInfo gap;
                    gap.text = m_schema.silenceLabel;
                    gap.start = from;
                    gap.duration = to - from;
                    // The label of an inserted segment is one of the declared phonemes, and the
                    // contract expects the phonemes of a word to cover it exactly, with the label
                    // of the segment in PhoneInfo::text. hfa, the other variant that inserts
                    // segments, reports the same shape.
                    gap.phones.push_back({m_schema.silenceLabel, from, to - from});
                    words.push_back(std::move(gap));
                };
                const auto refusesGaps = [this]() {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "this package declares no silence label, so it cannot report "
                                      "what the model placed no word over");
                };

                double frontier = 0;
                std::size_t next = 0;
                for (std::size_t word = 0; word < grid.wordCount(); ++word) {
                    std::vector<const Placed *> phones;
                    while (next < placed.size() && placed[next].word == word) {
                        phones.push_back(&placed[next]);
                        ++next;
                    }
                    if (phones.empty()) {
                        continue;
                    }

                    // The word keeps the frames its phones took, trimmed to the span: a graph may
                    // mask a frame the audio does not reach, and a word that ended past the span
                    // would claim time the caller never handed over.
                    const auto start = std::max(seconds(phones.front()->first), frontier);
                    const auto end = std::min(seconds(phones.back()->last), length);
                    if (end <= start) {
                        continue;
                    }
                    if (start > frontier) {
                        if (m_schema.silenceLabel.empty()) {
                            return refusesGaps();
                        }
                        silence(frontier, start);
                    }

                    AlignApi::WordInfo info;
                    info.text = grid.texts[word];
                    info.start = start;
                    info.duration = end - start;
                    // The phones cover the word exactly: each one runs to where the next begins,
                    // and the last to the end of the word. The decode leaves the frames it
                    // attributed to no token out of every span, and a word's phones are what it was
                    // written as, so a pause inside a word is absorbed by the phone after it rather
                    // than reported as a hole no phone of the contract can fill.
                    //
                    // The edges are the phones' own first frames, kept in order and held inside the
                    // word: a graph may mask a frame the audio does not reach, and a phone that
                    // leapt past the end would claim time the caller never handed over.
                    std::vector<double> edges;
                    edges.reserve(phones.size() + 1);
                    edges.push_back(start);
                    for (std::size_t index = 1; index < phones.size(); ++index) {
                        edges.push_back(
                            std::min(std::max(seconds(phones[index]->first), start), end));
                    }
                    edges.push_back(end);
                    for (std::size_t index = 0; index < phones.size(); ++index) {
                        if (edges[index + 1] <= edges[index]) {
                            continue;
                        }
                        AlignApi::PhoneInfo phone;
                        phone.text = phones[index]->label;
                        phone.start = edges[index];
                        phone.duration = edges[index + 1] - edges[index];
                        info.phones.push_back(std::move(phone));
                    }
                    if (info.phones.empty()) {
                        continue;
                    }
                    words.push_back(std::move(info));
                    frontier = end;
                }

                if (frontier < length) {
                    if (m_schema.silenceLabel.empty()) {
                        return refusesGaps();
                    }
                    silence(frontier, length);
                }

                // Onto the host's timeline, once: everything above is relative to the span the
                // model was given, and the contract's times are absolute.
                for (auto &word : words) {
                    word.start += anchor;
                    for (auto &phone : word.phones) {
                        phone.start += anchor;
                    }
                }
                return words;
            }

            // The label a result reports for one phoneme of a reading, spelled as the vocabulary
            // spells it.
            //
            // The vocabulary spells a symbol with the language it belongs to, and a result reports
            // the phonemes the declaration promised for the language it is aligning in, which it
            // spells without that language's prefix. So the prefix of the language in use is
            // dropped, and a phoneme borrowed from another language keeps its own: it says where it
            // came from because the declaration does not promise it for this one.
            std::string labelOf(const std::string &label, const std::string &language) const {
                const auto code = m_files.codes.find(language);
                if (code != m_files.codes.end()) {
                    const std::string prefix = code->second + LANGUAGE_SEPARATOR;
                    if (label.rfind(prefix, 0) == 0) {
                        return label.substr(prefix.size());
                    }
                }
                return label;
            }

            Models m_models;
            const AlignApi::Exports &m_schema;
            ModelFiles m_files;
        };

        srt::Expected<std::unique_ptr<TifaConfiguration>>
            readConfiguration(const srt::ContribSpec &spec) {
            const auto &value = spec.manifestConfiguration();
            if (!value.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the tifa configuration must be an object");
            }
            const auto object = value.toObject();
            if (auto checked = otter::manifest::rejectUnknownKeys(
                    object,
                    {"spectrogram", "model", "prepare", "score", "select", "config", "vocabulary",
                     "dictionaryCmn", "dictionaryYue", "dictionaryJpn", "dictionaryEng",
                     "languages"},
                    "the tifa configuration");
                !checked) {
                return checked.takeError();
            }

            auto result = std::make_unique<TifaConfiguration>();
            const auto directory = spec.declarationPath().parent_path();
            const std::pair<const char *, std::filesystem::path TifaConfiguration::*> files[] = {
                {"spectrogram", &TifaConfiguration::spectrogram},
                {"model",       &TifaConfiguration::model      },
                {"prepare",     &TifaConfiguration::prepare    },
                {"score",       &TifaConfiguration::score      },
                {"select",      &TifaConfiguration::select     },
                {"config",      &TifaConfiguration::config     },
                {"vocabulary",  &TifaConfiguration::vocabulary },
            };
            for (const auto &[key, member] : files) {
                const auto it = object.find(key);
                if (it == object.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      std::string("the tifa configuration needs a ") + key);
                }
                auto path = otter::manifest::readPath(it->second, directory, key);
                if (!path) {
                    return path.takeError();
                }
                result.get()->*member = path.take();
            }

            // Each dictionary names the language it holds rather than the model's code for it,
            // because the file is the package's own choice and two languages sharing a code would
            // otherwise have to share one. A key with no matching language is refused here, so a
            // misspelled one is not read as a language without a dictionary.
            for (const auto &[language, key] : DICTIONARY_KEYS) {
                const auto it = object.find(key);
                if (it == object.end()) {
                    continue;
                }
                auto path = otter::manifest::readPath(it->second, directory, key);
                if (!path) {
                    return path.takeError();
                }
                result->dictionaries.emplace(language, path.take());
            }

            const auto languages = object.find("languages");
            if (languages == object.end() || !languages->second.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the tifa configuration needs a languages object mapping the "
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
                                  "the tifa configuration declares no language");
            }

            return result;
        }

        // Checks a declaration against the model files it names.
        //
        // The contract syntax is the library's. Whether this package can honor what it declares is
        // checked here, where the declaration and the model's own files are both in hand. A
        // language the exports promise but no dictionary covers, or a phoneme list that disagrees
        // with the vocabulary, would otherwise surface as a failed execution long after the
        // package loaded, or, worse, as a result whose phonemes are in a notation the host was
        // not told to expect.
        srt::Expected<void> checkDeclaration(const AlignApi::Exports &declared,
                                             const TifaConfiguration &wiring) {
            auto facts = readModelFacts(wiring.config);
            if (!facts) {
                return facts.takeError();
            }
            auto vocabulary = otter::tifa::Vocabulary::read(wiring.vocabulary);
            if (!vocabulary) {
                return vocabulary.takeError();
            }
            const auto &model = *facts;
            const auto &symbols = *vocabulary;
            const auto shared = sharedSymbols(symbols);

            // What the model's own file says about its two widths is checked against the numbers
            // the graphs speak rather than against the declaration: a vocabulary whose symbols
            // reach past the classes the model scores would index past the end of its own table at
            // the first execution that used them.
            if (model.classes) {
                for (const auto &[label, number] : symbols.symbols()) {
                    if (number >= *model.classes) {
                        return srt::Error(srt::Error::InvalidFormat,
                                          "the vocabulary numbers " + label + " as " +
                                              std::to_string(number) +
                                              ", and the model's config.json scores " +
                                              std::to_string(*model.classes) + " classes");
                    }
                }
            }

            // The audio format is the same kind of promise and a worse one to leave unchecked: the
            // host prepares the audio exactly as declared, so a rate these graphs were not exported
            // at does not fail: it aligns at the wrong speed, which reads as a plausible result.
            if (declared.sampleRate != model.sampleRate) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the tifa variant runs at " + std::to_string(model.sampleRate) +
                                      " Hz; the exports declare " +
                                      std::to_string(declared.sampleRate));
            }
            if (declared.channelCount != 1) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the tifa variant feeds its model one channel, and the exports "
                                  "declare " +
                                      std::to_string(declared.channelCount));
            }
            if (!declared.languages.empty() &&
                std::find_if(declared.languages.begin(), declared.languages.end(),
                             [&declared](const AlignApi::LanguageInfo &entry) {
                                 return entry.language == declared.defaultLanguage;
                             }) == declared.languages.end()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the exports name " + declared.defaultLanguage +
                                      " as their default language, and no language they list "
                                      "carries it");
            }

            std::set<std::string> promised;
            std::set<std::string> seen;
            for (const auto &entry : declared.languages) {
                const auto &language = entry.language;
                // A scheme this variant does not read is refused rather than guessed at: the same
                // language in another notation has other phonemes, and a dictionary read as if it
                // were in that notation would align a different phoneme set.
                if (!isKnownScheme(entry.scheme)) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the tifa variant reads the schemes pinyin, jyutping, romaji "
                                      "and arpabet, and the exports declare " +
                                          entry.scheme + " for " + language);
                }
                if (!seen.insert(language + LANGUAGE_SEPARATOR + entry.scheme).second) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the exports list " + language + " with the scheme " +
                                          entry.scheme + " more than once");
                }
                const auto code = wiring.languages.find(language);
                if (code == wiring.languages.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the exports list the language " + language +
                                          " but the configuration gives it no model code");
                }
                const auto *key = dictionaryKey(language);
                if (key == nullptr || wiring.dictionaries.count(language) == 0) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the exports list " + language +
                                          ", and the configuration carries no " +
                                          (key != nullptr ? std::string(key)
                                                          : std::string("dictionary for it, since "
                                                                        "this variant covers cmn, "
                                                                        "yue, jpn and eng")));
                }
                // Read rather than merely found: a dictionary that cannot be read is a language
                // this package cannot align, however sound its declaration looks.
                auto dictionary = otter::tifa::Dictionary::load(wiring.dictionaries.at(language));
                if (!dictionary) {
                    return dictionary.takeError().withContext("cannot read the " + language +
                                                              " dictionary");
                }

                // The declared phonemes and the model's own must agree, in the one direction that
                // is a promise about this language and in the other that is a promise about the
                // vocabulary. A symbol without a language prefix belongs to no language in
                // particular: the exporter merges the ones two languages share, so it may be
                // listed by any of them. Every other listed phoneme has to be one this language's
                // symbols actually hold.
                const auto emitted = emittedOf(symbols, code->second);
                if (emitted.empty()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the model vocabulary holds no phoneme of the language " +
                                          language + " (" + code->second + ")");
                }
                const std::set<std::string> declaredHere(entry.phonemes.begin(),
                                                         entry.phonemes.end());
                std::set<std::string> notListed;
                std::set_difference(emitted.begin(), emitted.end(), declaredHere.begin(),
                                    declaredHere.end(), std::inserter(notListed, notListed.end()));
                std::set<std::string> unknown;
                for (const auto &label : declaredHere) {
                    if (emitted.count(label) == 0 && shared.count(label) == 0) {
                        unknown.insert(label);
                    }
                }
                if (!notListed.empty() || !unknown.empty()) {
                    return srt::Error(
                        srt::Error::InvalidFormat,
                        "the phonemes the exports list for " + language +
                            " differ from the phonemes in the model vocabulary for " +
                            code->second +
                            (notListed.empty() ? "" : "; not listed:" + spelled(notListed)) +
                            (unknown.empty() ? "" : "; listed but unknown:" + spelled(unknown)));
                }
                promised.insert(entry.phonemes.begin(), entry.phonemes.end());
            }

            // The silence label is this variant's own spelling for a stretch of audio that carries
            // no token, since the model has none of its own. It therefore cannot also be a phoneme
            // of a language the exports list: a host filtering the gaps out by that label would
            // drop the phoneme it collides with as well.
            if (!declared.silenceLabel.empty() && promised.count(declared.silenceLabel) != 0) {
                return srt::Error(
                    srt::Error::InvalidFormat,
                    "the exports name " + declared.silenceLabel +
                        " as the silence label, which is also a phoneme of a language "
                        "they list");
            }

            // The model has no non-speech head: it cannot report a breath or a cough, so promising
            // one would make a search that found nothing and a search that never ran the same
            // answer.
            if (!declared.nonSpeechPhonemes.empty()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the tifa model detects no non-speech sound, so the exports "
                                  "cannot promise the phoneme " +
                                      declared.nonSpeechPhonemes.front());
            }
            return srt::Expected<void>();
        }

    }

    srt::Expected<std::unique_ptr<srt::ContribExports>>
        TifaInterpreter::createExports(const srt::ContribSpec &spec) const {
        auto schema = AlignApi::Exports::read(spec);
        if (!schema) {
            return schema.takeError();
        }
        auto configuration = readConfiguration(spec);
        if (!configuration) {
            return configuration.takeError();
        }
        auto checked = checkDeclaration(**schema, **configuration);
        if (!checked) {
            return checked.takeError();
        }
        return std::unique_ptr<srt::ContribExports>(schema.take().release());
    }

    srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
        TifaInterpreter::createConfiguration(const srt::ContribSpec &spec) const {
        auto result = readConfiguration(spec);
        if (!result) {
            return result.takeError();
        }
        return std::unique_ptr<srt::ContribConfiguration>(result.take().release());
    }

    srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
        TifaInterpreter::createInference(srt::InferenceSpec &spec,
                                         const srt::ContribImportOptions &,
                                         const srt::InferenceRuntimeOptions &) {
        const auto configuration =
            spec.configuration() ? spec.configuration()->as<TifaConfiguration>() : nullptr;
        const auto schema = spec.exports() ? spec.exports()->as<AlignApi::Exports>() : nullptr;
        if (configuration == nullptr || schema == nullptr) {
            return srt::Error(otter::AnalysisError::Internal,
                              "this declaration carries no tifa configuration");
        }

        auto files = ModelFiles::load(*configuration);
        if (!files) {
            return files.takeError();
        }

        OpenedModels opened;
        // Where each graph is and whether it runs on the CPU rather than on the accelerator.
        const std::tuple<std::filesystem::path,
                         std::unique_ptr<ds::InferenceSession> OpenedModels::*, bool>
            graphs[] = {
                {configuration->spectrogram, &OpenedModels::spectrogram, false},
                {configuration->model,       &OpenedModels::model,       false},
                {configuration->prepare,     &OpenedModels::prepare,     true },
                {configuration->score,       &OpenedModels::score,       true },
                {configuration->select,      &OpenedModels::select,      true },
        };
        for (const auto &[path, member, useCpu] : graphs) {
            auto session = otter::onnx::openSession(spec, path, "tifa graph", useCpu);
            if (!session) {
                return session.takeError();
            }
            opened.*member = session.take();
        }

        return std::unique_ptr<srt::InferenceExecutive>(
            new TifaExecutive(spec, std::move(opened), *schema, files.take()));
    }

}
