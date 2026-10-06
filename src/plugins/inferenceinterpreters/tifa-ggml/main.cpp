// The forced alignment provider backed by the tifa.cpp ggml engine, driven as an external process.
//
// The ONNX variant of this contract runs five graphs and keeps the decode on the host: the
// candidate grid, the Viterbi and the reading choices are all spelled out beside the sessions.
// Here the whole pipeline lives inside the engine's command line tool, which the provider launches
// once per execution: the span and the lyrics travel as files, the tool answers with a TextGrid of
// the three tiers its dataset workflow writes, and what this variant adds on top is the contract
// side — the words of the texts tier, the phones of the phones tier, and the gaps the tool left to
// nothing reported as words of the declaration's silence label, the same shape the ONNX variant
// reports.
//
// This port differs from the ONNX variant in the same two observable ways as its note sibling:
// the tool call is monolithic, so progress reports 0 and then 1, and a stop terminates the tool.
// And the readings of a polyphonic word are not scored against the audio: the first pronunciation
// the G2P offers is the one aligned, where the ONNX variant lets its score graph choose. A caller
// who needs another reading writes the lyrics as the scheme spelling that dictionary entry carries.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/SVS/InferenceInterpreterPlugin.h>
#include <synthrt/Support/Expected.h>
#include <synthrt/Support/JSON.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/Provider/AnalysisInput.h>
#include <otter/Analysis/Provider/AnalysisInterpreter.h>
#include <otter/Api/Align/1/AlignApiL1.h>
#include <otter/Support/ManifestValues.h>

#include "../external/CliProcess.h"

namespace AlignApi = otter::Api::Align::L1;

namespace {

    constexpr char VARIANT[] = "tifa-ggml";

    /// The name the span travels under inside the scratch directory. The tool names its outputs
    /// after the audio file, so the fixed stem is what tells the provider where the TextGrid lands.
    constexpr char SPAN_STEM[] = "span";

    /// The configuration block of this variant: the engine's command line tool, the model it runs,
    /// the directory its G2P dictionaries live in, and the map from the host's language identifiers
    /// to the engine's own codes. The audio format and the languages are contract facts and are
    /// declared in exports.
    class TifaGgmlConfiguration : public srt::ContribConfiguration {
    public:
        TifaGgmlConfiguration()
            : srt::ContribConfiguration(AlignApi::API_INTERFACE, VARIANT, AlignApi::API_LEVEL) {
        }

        /// The engine's command line tool, as shipped in its release.
        std::filesystem::path cli;

        /// The GGUF model the tool runs.
        std::filesystem::path model;

        /// The directory the tool resolves its G2P dictionary references against. Empty when the
        /// package ships no dictionaries, in which case the tool reads them from beside the model,
        /// as its own tooling does.
        std::filesystem::path dictionaries;

        /// Maps the identifiers the exports list to the codes the engine speaks. A package whose
        /// model carries one unprefixed vocabulary may map nothing, and the engine then reads the
        /// lyrics without a language.
        std::map<std::string, std::string> languages;
    };

    /// One interval of one TextGrid tier, as the tool writes them: half open, in seconds.
    struct GridInterval {
        double start = 0;
        double end = 0;
        std::string text;
    };

    /// The two tiers of the tool's TextGrid that the contract is built from: the semantic words the
    /// caller wrote, and the phones those words were pronounced with.
    struct GridTiers {
        std::vector<GridInterval> texts;
        std::vector<GridInterval> phones;
    };

    /// Reads the intervals of the `texts` and `phones` tiers of a TextGrid.
    ///
    /// The tool writes the long Praat text format, whose intervals are one `xmin`, `xmax` and
    /// `text` triple after another inside the tier they belong to; the tier a triple belongs to is
    /// the one whose `name` line came before it. A line the format does not define is skipped, so
    /// a tier this variant does not read passes through.
    ///
    /// \return The tiers, or an \c AnalysisError::ModelFailed error if the file cannot be read or
    ///         holds no tier pair the tool could have written.
    srt::Expected<GridTiers> readTextGrid(const std::filesystem::path &path) {
        std::FILE *file = std::fopen(path.string().c_str(), "rb");
        if (file == nullptr) {
            return srt::Error(otter::AnalysisError::ModelFailed,
                              "cannot read the alignment the tifa CLI wrote: " + path.string());
        }
        GridTiers tiers;
        std::string tierName;
        // The tier the pending triple belongs to, remembered when the triple starts: the next
        // tier's header arrives before its own first triple flushes the previous tier's last one.
        std::string pendingTier;
        // Whether an interval of the current tier is open. A tier states its own xmin and xmax in
        // its header, and those lines are not an interval; only a triple that follows an
        // "intervals [" line is one.
        bool inInterval = false;
        std::optional<double> xmin;
        std::optional<double> xmax;
        std::string text;
        const auto flush = [&tiers, &pendingTier, &xmin, &xmax, &text] {
            if (!xmin || !xmax) {
                return;
            }
            GridInterval interval{*xmin, *xmax, text};
            if (pendingTier == "texts") {
                tiers.texts.push_back(std::move(interval));
            } else if (pendingTier == "phones") {
                tiers.phones.push_back(std::move(interval));
            }
            xmin.reset();
            xmax.reset();
            text.clear();
        };
        char line[1024];
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            std::string_view view(line);
            // The fields are matched at the start of the line, after its indentation: a field name
            // that merely occurs inside another field's value, quoted text above all, is not a
            // field.
            const auto first = view.find_first_not_of(" \t");
            if (first != std::string_view::npos) {
                view.remove_prefix(first);
            }
            const auto begins = [&view](std::string_view field) {
                return view.rfind(field, 0) == 0;
            };
            const auto number = [](std::string_view field) {
                try {
                    return std::stod(std::string(field));
                } catch (const std::exception &) {
                    return 0.0;
                }
            };
            if (begins("name = ")) {
                flush();
                inInterval = false;
                const auto begin = view.find('"');
                const auto end = view.rfind('"');
                tierName = begin != std::string_view::npos && end != begin
                               ? std::string(view.substr(begin + 1, end - begin - 1))
                               : std::string();
            } else if (begins("intervals [")) {
                // A new interval of one tier starts; the triple before it is complete.
                flush();
                inInterval = true;
            } else if (!inInterval) {
                // The tier header and the file header state xmin and xmax as well, and neither is
                // an interval of anything.
                continue;
            } else if (begins("xmin = ")) {
                pendingTier = tierName;
                xmin = number(view.substr(view.find('=') + 1));
            } else if (begins("xmax = ")) {
                xmax = number(view.substr(view.find('=') + 1));
            } else if (begins("text = ")) {
                const auto begin = view.find('"');
                const auto end = view.rfind('"');
                text = begin != std::string_view::npos && end != begin
                           ? std::string(view.substr(begin + 1, end - begin - 1))
                           : std::string();
            }
        }
        std::fclose(file);
        // The last interval of the last tier, which no following line closes.
        flush();
        if (tiers.texts.empty() && tiers.phones.empty()) {
            return srt::Error(otter::AnalysisError::ModelFailed,
                              "the alignment the tifa CLI wrote holds no tiers: " + path.string());
        }
        return tiers;
    }

    class TifaGgmlExecutive : public AlignApi::AlignExecutive {
    public:
        TifaGgmlExecutive(srt::InferenceSpec &spec, const TifaGgmlConfiguration &configuration,
                          const AlignApi::AlignSchema &schema)
            : AlignExecutive(spec), m_configuration(configuration), m_schema(schema) {
        }

        ~TifaGgmlExecutive() override {
            // The body runs the tool, so an execution still in flight has to be stopped and waited
            // out before the members of this derived part are destroyed, before the base part and
            // with it the task.
            (void) stop();
            (void) waitForFinished();
        }

        /// Requests cancellation and also kills the running tool, because the tool call is what a
        /// stopped execution is blocked on. The handle and the pipes are reaped by the execution
        /// thread, which is the only thread that owns them.
        srt::Expected<void> stop() override {
            (void) AlignExecutive::stop();
            (void) m_active.requestCancel();
            return srt::Expected<void>();
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
            const auto &waveform = prepared.take();
            if (waveform.empty()) {
                return srt::Error(srt::Error::InvalidArgument, "the span carries no audio");
            }

            // An aligner that is not told what to look for has nothing to align: answering with a
            // transcription instead would be a different contract.
            if (input.lyrics.empty()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "an alignment needs to know what is sung, and lyrics is empty");
            }

            const std::string language = input.language.value_or(m_schema.defaultLanguage);
            if (language.empty()) {
                return srt::Error(srt::Error::InvalidArgument,
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
                    return srt::Error(srt::Error::InvalidArgument,
                                      "this package declares " + language +
                                          " with more than one scheme, so the caller must specify "
                                          "a scheme");
                }
                entry = &candidate;
            }
            if (entry == nullptr) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "the caller asked for the language " + language +
                                      (input.scheme ? " with the scheme " + *input.scheme : "") +
                                      ", which this model cannot align");
            }
            const auto code = m_configuration.languages.find(language);
            if (code == m_configuration.languages.end()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "this package carries no engine language for " + language);
            }

            // The engine detects no non-speech sound of its own, so there is nothing to switch on
            // and nothing a request for one could mean. A caller asking for a label would
            // otherwise be told that none was found, which it cannot tell from there being none.
            if (!input.nonSpeechPhonemes.empty()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "the caller asked for the non-speech phoneme " +
                                      input.nonSpeechPhonemes.front() +
                                      ", which this model cannot report");
            }

            const auto report = [this, &input](double value) {
                if (input.progress && !input.progress(value)) {
                    // The callback answered that the execution does not continue, so the stop is
                    // requested here and the calls to checkCancelled() below report it.
                    (void) stop();
                }
            };
            report(0);
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }

            // The tool call covers the G2P, the spectrogram, the model and the decode, and offers
            // no way in for a stop request, so the execution reports its progress in few points
            // and a stop that arrives during the call terminates the tool.
            auto produced = align(waveform, input.lyrics, code->second);
            if (!produced) {
                return produced.takeError();
            }
            report(0.7);
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }

            // Onto the words the caller wrote, and onto the host's timeline.
            const double length = audio.duration();
            auto words = place(produced.take(), length, audio.startTime);
            if (!words) {
                return words.takeError();
            }

            auto result = std::make_unique<AlignApi::AlignResult>();
            result->language = entry->language;
            result->scheme = entry->scheme;
            result->words = words.take();
            report(1);
            return result;
        }

    private:
        // Returns whether a stop has been requested for the current execution.
        srt::Expected<void> checkCancelled() const {
            if (cancelled()) {
                return otter::cancelledError();
            }
            return srt::Expected<void>();
        }

        // Runs the tool over one span and reads back the alignment it wrote.
        //
        // The span and the lyrics travel as files of a scratch directory that is removed when the
        // execution is over. The tool reports the failures of its own layer through its exit code
        // and its standard error, which goes to the host's; a tool that died mid-run is reported
        // as a model failure, unless the execution was cancelled, whose stop is the reason it died.
        srt::Expected<GridTiers> align(const otter::PreparedSamples &waveform,
                                       const std::string &lyrics, const std::string &engineCode) {
            auto scratch = otter::cli::makeScratchDirectory("otter-tifa-ggml");
            if (!scratch) {
                return scratch.takeError();
            }
            const auto directory = scratch.take();
            const auto spanFile = directory / (std::string(SPAN_STEM) + ".wav");
            const auto lyricsFile = directory / "lyrics.txt";
            auto wrote = otter::cli::writeWav(spanFile, {waveform.begin(), waveform.end()},
                                              m_schema.sampleRate);
            if (wrote) {
                wrote = [&] {
                    std::FILE *file = std::fopen(lyricsFile.string().c_str(), "wb");
                    if (file == nullptr) {
                        return srt::Expected<void>(srt::Error(srt::Error::FileNotOpen,
                                                              "cannot write " +
                                                                  lyricsFile.string()));
                    }
                    std::fwrite(lyrics.data(), 1, lyrics.size(), file);
                    std::fclose(file);
                    return srt::Expected<void>();
                }();
            }
            if (!wrote) {
                otter::cli::removeScratchDirectory(directory);
                return wrote.takeError();
            }

            std::vector<std::string> commandLine = {m_configuration.cli.string(), "align",
                                                    spanFile.string(), "-m",
                                                    m_configuration.model.string()};
            commandLine.insert(commandLine.end(), {"--text-file", lyricsFile.string()});
            if (!engineCode.empty()) {
                commandLine.insert(commandLine.end(), {"-l", engineCode});
            }
            if (!m_configuration.dictionaries.empty()) {
                commandLine.insert(commandLine.end(), {"--dict-dir",
                                                       m_configuration.dictionaries.string()});
            }
            commandLine.insert(commandLine.end(), {"--skip-handling", "omit", "--output-formats",
                                                   "textgrid", "-o", directory.string()});
            auto spawned = m_active.start(commandLine);
            if (spawned) {
                auto code = m_active.wait();
                if (!code) {
                    spawned = code.takeError();
                } else if (code.take() != 0) {
                    spawned = srt::Error(otter::AnalysisError::ModelFailed,
                                         "the tifa CLI exited with an error; its notes are on the "
                                         "host's standard error");
                }
            }
            // The pipes are reaped here, on the thread that owns them, whether the tool finished,
            // failed or was killed by a stop.
            m_active.terminate();
            if (!spawned) {
                otter::cli::removeScratchDirectory(directory);
                if (cancelled()) {
                    return otter::cancelledError();
                }
                return spawned.takeError();
            }

            auto tiers = readTextGrid(directory / (std::string(SPAN_STEM) + ".TextGrid"));
            otter::cli::removeScratchDirectory(directory);
            return tiers;
        }

        // Turns the tool's two tiers into the words the contract asks for.
        //
        // The decode places tokens, not words: one word may be several phonemes, and the stretch
        // of audio the decode attributed to no token belongs to no word at all. The texts tier
        // already gathers the phones back onto the word they were written for, so the placement
        // walks the word intervals in order and reads the phones each one holds, and every stretch
        // between two words, the ends of the span included, becomes a word of its own carrying the
        // declaration's silence label. That is what makes the result cover the span: a host that
        // wants only what was sung filters on that label rather than reconstructing the gaps.
        srt::Expected<std::vector<AlignApi::WordInfo>>
            place(const GridTiers &tiers, double length, double anchor) const {
            // The words of the result, in the order the texts tier wrote them. An interval the
            // tool left empty is a stretch of no word, which the silence below fills; an interval
            // its phones did not reach is a word of no audio, which is dropped, because the
            // contract has no way to express a word of no length.
            struct Word {
                std::string text;
                std::vector<const GridInterval *> phones;
            };
            std::vector<Word> words;
            for (const auto &interval : tiers.texts) {
                if (interval.text.empty()) {
                    continue;
                }
                Word word{interval.text, {}};
                for (const auto &phone : tiers.phones) {
                    // A phone belongs to the word whose interval holds its middle. The tool tiles
                    // each word with its own phones, so the middle never straddles two of them.
                    const double middle = (phone.start + phone.end) / 2;
                    if (interval.start <= middle && middle <= interval.end) {
                        word.phones.push_back(&phone);
                    }
                }
                if (!word.phones.empty()) {
                    words.push_back(std::move(word));
                }
            }

            std::vector<AlignApi::WordInfo> placed;
            const auto silence = [this, &placed](double from, double to) {
                AlignApi::WordInfo gap;
                gap.text = m_schema.silenceLabel;
                gap.start = from;
                gap.duration = to - from;
                // The label of an inserted segment is one of the declared phonemes, and the
                // contract expects the phonemes of a word to cover it exactly, with the label of
                // the segment in PhoneInfo::text.
                gap.phones.push_back({m_schema.silenceLabel, from, to - from});
                placed.push_back(std::move(gap));
            };
            const auto refusesGaps = [this]() {
                return srt::Error(srt::Error::InvalidFormat,
                                  "this package declares no silence label, so it cannot report "
                                  "what the model placed no word over");
            };

            double frontier = 0;
            for (const auto &word : words) {
                // The word keeps the frames its phones took, trimmed to the span: a decode may
                // reach past the audio, and a word that ended past the span would claim time the
                // caller never handed over.
                const auto start = std::max(word.phones.front()->start, frontier);
                const auto end = std::min(word.phones.back()->end, length);
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
                info.text = word.text;
                info.start = start;
                info.duration = end - start;
                // The phones cover the word exactly: each one runs to where the next begins, and
                // the last to the end of the word.
                std::vector<double> edges;
                edges.reserve(word.phones.size() + 1);
                edges.push_back(start);
                for (std::size_t index = 1; index < word.phones.size(); ++index) {
                    edges.push_back(std::min(std::max(word.phones[index]->start, start), end));
                }
                edges.push_back(end);
                for (std::size_t index = 0; index < word.phones.size(); ++index) {
                    if (edges[index + 1] <= edges[index]) {
                        continue;
                    }
                    AlignApi::PhoneInfo phone;
                    phone.text = word.phones[index]->text;
                    phone.start = edges[index];
                    phone.duration = edges[index + 1] - edges[index];
                    info.phones.push_back(std::move(phone));
                }
                if (info.phones.empty()) {
                    continue;
                }
                placed.push_back(std::move(info));
                frontier = end;
            }

            if (frontier < length) {
                if (m_schema.silenceLabel.empty()) {
                    return refusesGaps();
                }
                silence(frontier, length);
            }

            // Onto the host's timeline, once: everything above is relative to the span the model
            // was given, and the contract's times are absolute.
            for (auto &word : placed) {
                word.start += anchor;
                for (auto &phone : word.phones) {
                    phone.start += anchor;
                }
            }
            return placed;
        }

        // The tool of the execution in flight, if any. Every execution starts it and reaps it on
        // its own thread, and a stop from another thread reaches it through requestCancel() only.
        otter::cli::Process m_active;

        // Owned by the spec, which outlives every executive created from it.
        const TifaGgmlConfiguration &m_configuration;
        const AlignApi::AlignSchema &m_schema;
    };

    /// Checks the files of a package and hands out analyzers.
    class TifaGgmlInterpreter : public otter::AnalysisInterpreter {
    public:
        TifaGgmlInterpreter() = default;

        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override {
            auto schema = AlignApi::readAlignSchema(spec, VARIANT);
            if (!schema) {
                return schema.takeError();
            }
            auto configuration = readConfiguration(spec);
            if (!configuration) {
                return configuration.takeError();
            }
            const auto &declared = **schema;
            const auto &wiring = **configuration;
            // The library checks the contract syntax. Whether this package can honor its
            // declaration is checked here, where both blocks are available. The model's own facts
            // live in its GGUF file, which no package load should pay for reading, so the phoneme
            // lists are checked against the engine at the first execution instead.
            if (declared.languages.empty()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the tifa-ggml variant aligns in a language, so the exports "
                                  "must declare at least one");
            }
            if (declared.channelCount != 1) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the tifa-ggml variant feeds its model one channel, and the "
                                  "exports declare " +
                                      std::to_string(declared.channelCount));
            }
            for (const auto &entry : declared.languages) {
                if (wiring.languages.find(entry.language) == wiring.languages.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the exports list the language " + entry.language +
                                          " but the configuration gives it no engine code");
                }
            }
            // The engine detects no non-speech sound: it cannot report a breath or a cough, so
            // promising one would make "none found" and "nothing was looked for" the same answer.
            if (!declared.nonSpeechPhonemes.empty()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the tifa-ggml model detects no non-speech sound, so the exports "
                                  "cannot promise the phoneme " +
                                      declared.nonSpeechPhonemes.front());
            }
            return std::unique_ptr<srt::ContribExports>(schema.take().release());
        }

        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override {
            auto result = readConfiguration(spec);
            if (!result) {
                return result.takeError();
            }
            return std::unique_ptr<srt::ContribConfiguration>(result.take().release());
        }

        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override {
            const auto configuration =
                spec.configuration() ? spec.configuration()->as<TifaGgmlConfiguration>() : nullptr;
            const auto schema =
                spec.exports() ? spec.exports()->as<AlignApi::AlignSchema>() : nullptr;
            if (configuration == nullptr || schema == nullptr) {
                return srt::Error(otter::AnalysisError::Internal,
                                  "this declaration carries no tifa-ggml configuration");
            }
            auto checked = checkFiles(*configuration);
            if (!checked) {
                return checked.takeError();
            }
            return std::unique_ptr<srt::InferenceExecutive>(
                new TifaGgmlExecutive(spec, *configuration, *schema));
        }

    private:
        // The engine and the model are files the declaration names. A package whose files are
        // missing is one that cannot run, however sound its declaration looks, and the refusal
        // belongs here, where the analyzer is created, rather than at the first execution.
        static srt::Expected<void> checkFiles(const TifaGgmlConfiguration &configuration) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(configuration.cli, error)) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "cannot find the tifa CLI at " + configuration.cli.string());
            }
            if (!std::filesystem::is_regular_file(configuration.model, error)) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "cannot find the tifa model at " + configuration.model.string());
            }
            if (!configuration.dictionaries.empty() &&
                !std::filesystem::is_directory(configuration.dictionaries, error)) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "cannot find the G2P dictionaries at " +
                                      configuration.dictionaries.string());
            }
            return srt::Expected<void>();
        }

        static srt::Expected<std::unique_ptr<TifaGgmlConfiguration>>
            readConfiguration(const srt::ContribSpec &spec) {
            const auto &value = spec.manifestConfiguration();
            if (!value.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the tifa-ggml configuration must be an object");
            }
            const auto object = value.toObject();
            if (auto checked = otter::manifest::rejectUnknownKeys(
                    object, {"cli", "model", "dictionaries", "languages"},
                    "the tifa-ggml configuration");
                !checked) {
                return checked.takeError();
            }

            auto result = std::make_unique<TifaGgmlConfiguration>();

            const auto directory = spec.declarationPath().parent_path();
            const std::pair<const char *, std::filesystem::path TifaGgmlConfiguration::*> files[] = {
                {"cli",   &TifaGgmlConfiguration::cli  },
                {"model", &TifaGgmlConfiguration::model},
            };
            for (const auto &[key, member] : files) {
                const auto it = object.find(key);
                if (it == object.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      std::string("the tifa-ggml configuration needs a ") + key);
                }
                auto path = otter::manifest::readPath(it->second, directory, key);
                if (!path) {
                    return path.takeError();
                }
                result.get()->*member = path.take();
            }

            if (const auto it = object.find("dictionaries"); it != object.end()) {
                auto path = otter::manifest::readPath(it->second, directory, "dictionaries");
                if (!path) {
                    return path.takeError();
                }
                result->dictionaries = path.take();
            }

            if (const auto it = object.find("languages"); it != object.end()) {
                if (!it->second.isObject()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "languages must map identifiers to the engine's codes");
                }
                for (const auto &[name, code] : it->second.toObject()) {
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
            }
            return result;
        }
    };

    class TifaGgmlPlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(std::string_view interfaceName, int level, std::string_view variant) override {
            if (interfaceName != AlignApi::API_INTERFACE || level != AlignApi::API_LEVEL ||
                variant != VARIANT) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this plugin serves the " +
                                      std::string(AlignApi::API_INTERFACE) +
                                      " contract at level " + std::to_string(AlignApi::API_LEVEL) +
                                      " only, in the variant " + VARIANT);
            }
            return std::unique_ptr<srt::ContribInterpreter>(new TifaGgmlInterpreter());
        }
    };

}

STDC_EXPORT_PLUGIN(TifaGgmlPlugin)
