// An interpreter that implements the F0, Note and Align Level 1 contracts without any model.
//
// The stub allows the parts of otter unrelated to model execution, such as package loading,
// interpreter discovery, declaration reading, and executive creation and cancellation, to be
// tested in isolation. A failure of the stub indicates a framework defect. A failure of a shipped
// provider while the stub passes indicates a model or driver defect. This separation is the purpose
// of the stub.
//
// The results are computed arithmetically rather than by analysis: a tone at a fixed frequency for
// F0, one note per declared beat for Note, and the words of the lyrics in equal spans for Align.
// The results are deterministic, as a test requires.
//
// The stub reads its declaration in the same way as a shipped provider: the audio format and the
// knobs come from exports through the readers of the library, and only the stub settings come from
// configuration. A stub less strict than the shipped providers would let a contract test pass
// for behavior that no shipped analyzer has.

#include <algorithm>
#include <chrono>
#include <sstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/SVS/InferenceInterpreterPlugin.h>
#include <synthrt/Support/Expected.h>
#include <synthrt/Support/JSON.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/AnalysisInput.h>
#include <otter/Analysis/AnalysisInterpreter.h>
#include <otter/Api/Align/1/AlignApiL1.h>
#include <otter/Api/F0/1/F0ApiL1.h>
#include <otter/Api/Note/1/NoteApiL1.h>
#include <otter/Support/F0Curve.h>
#include <otter/Support/KnownNotes.h>
#include <otter/Support/ManifestValues.h>

namespace AlignApi = otter::Api::Align::L1;
namespace F0Api = otter::Api::F0::L1;
namespace NoteApi = otter::Api::Note::L1;
namespace CommonApi = otter::Api::Common::L1;

namespace {

    constexpr char VARIANT[] = "stub";

    /// The values used if the declaration does not honor the corresponding knob. They equal the
    /// fallback values of the shipped variants, rmvpe for F0 and game for Note, so that the stub
    /// interprets a declaration in the same way as the variant it replaces.
    constexpr double FALLBACK_VOICING_THRESHOLD = 0.03;
    constexpr bool FALLBACK_INTERPOLATE = true;
    constexpr int FALLBACK_STEPS = 8;
    constexpr double FALLBACK_BOUNDARY_THRESHOLD = 0.2;
    constexpr double FALLBACK_BOUNDARY_RADIUS = 0.02;
    constexpr double FALLBACK_NOTE_THRESHOLD = 0.2;
    constexpr double FALLBACK_NOTE_PRESENCE_CUTOFF = 0.5;
    constexpr double FALLBACK_NON_SPEECH_THRESHOLD = 0.5;
    constexpr double FALLBACK_NON_SPEECH_MIN_DURATION = 0.1;
    constexpr double FALLBACK_GAP_FILL = 0.1;

    /// Blocks in small steps so a test can observe a running execution and cancel it.
    ///
    /// The delay is declared per contribution and defaults to zero, so a test that requires only
    /// a result incurs no delay.
    ///
    /// \return \c true if the delay elapsed; \c false if a stop was requested.
    template <class Cancelled>
    bool sleepUnlessStopped(double seconds, Cancelled cancelled) {
        const auto steps = static_cast<int>(seconds * 200);
        for (int i = 0; i < steps; ++i) {
            if (cancelled()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return !cancelled();
    }

    /// The settings a stub declaration may carry in its configuration.
    struct StubSettings {
        double delay = 0;
        double frequency = 440;
        double beat = 0.5;

        /// Makes the interpreter return an executive that implements no contract, as a defective
        /// third-party interpreter could, and which createAnalyzer() must reject.
        bool foreignExecutive = false;
    };

    /// The stub's configuration block, stored on the spec like the configuration of any variant.
    class StubConfiguration : public srt::ContribConfiguration {
    public:
        StubConfiguration(const char *interfaceName, int level, StubSettings settings)
            : srt::ContribConfiguration(interfaceName, VARIANT, level), settings(settings) {
        }

        StubSettings settings;
    };

    /// The base of the stub executives: the schema that supplies the audio format and the knobs,
    /// the settings, and the shared validation call. The lifecycle is inherited from the contract.
    template <class Contract, class Schema>
    class StubExecutive : public Contract {
    public:
        StubExecutive(srt::InferenceSpec &spec, const Schema &schema, StubSettings settings)
            : Contract(spec), m_schema(schema), m_settings(settings) {
        }

        ~StubExecutive() {
            (void) this->stop();
            (void) this->waitForFinished();
        }

    protected:
        /// Validates the audio exactly as a shipped provider does, through the same library call.
        ///
        /// \return An empty value if the audio matches the declared format; otherwise the error of
        ///         otter::prepareSamples().
        srt::Expected<void> validate(const CommonApi::AudioSegment &audio) const {
            auto prepared = otter::prepareSamples(audio, m_schema.sampleRate, m_schema.channelCount,
                                                  m_schema.maxSegmentDuration);
            if (!prepared) {
                return prepared.takeError();
            }
            return srt::Expected<void>();
        }

        /// Owned by the spec, which outlives every executive created from it.
        const Schema &m_schema;
        StubSettings m_settings;
    };

    class StubF0Executive : public StubExecutive<F0Api::F0Executive, F0Api::F0Schema> {
    public:
        using StubExecutive::StubExecutive;

    protected:
        srt::Expected<std::unique_ptr<F0Api::F0Result>>
            run(const F0Api::F0StartInput &input) override {
            if (auto checked = validate(input.audio); !checked) {
                return checked.takeError();
            }
            if (auto chosen = otter::chooseKnob(input.voicingThreshold, m_schema.voicingThreshold,
                                                FALLBACK_VOICING_THRESHOLD, "voicingThreshold");
                !chosen) {
                return chosen.takeError();
            }
            if (input.progress) {
                input.progress(0);
            }
            if (!sleepUnlessStopped(m_settings.delay, [this] { return cancelled(); })) {
                return otter::cancelledError();
            }

            const auto frames =
                static_cast<std::size_t>(input.audio.duration() / m_schema.interval);
            auto result = std::make_unique<F0Api::F0Result>();
            result->startTime = input.audio.startTime;
            result->interval = m_schema.interval;
            result->f0.resize(frames);
            result->voiced.resize(frames);
            // The first half of every second is voiced and the rest unvoiced, so that a test
            // observes both kinds of frame and can distinguish enabled from disabled interpolation.
            for (std::size_t i = 0; i < frames; ++i) {
                const bool voiced = (i % 100) < 50;
                result->voiced[i] = voiced ? 1 : 0;
                result->f0[i] = voiced ? static_cast<float>(m_settings.frequency) : 0.0f;
            }
            if (otter::chooseKnob(input.interpolateUnvoiced, m_schema.interpolateUnvoiced,
                                  FALLBACK_INTERPOLATE)) {
                otter::support::interpolateUnvoiced(result->f0, result->voiced);
            }
            if (input.progress) {
                input.progress(1);
            }
            return result;
        }
    };

    class StubNoteExecutive : public StubExecutive<NoteApi::NoteExecutive, NoteApi::NoteSchema> {
    public:
        using StubExecutive::StubExecutive;

    protected:
        srt::Expected<std::unique_ptr<NoteApi::NoteResult>>
            run(const NoteApi::NoteStartInput &input) override {
            if (auto checked = validate(input.audio); !checked) {
                return checked.takeError();
            }
            // A language named by the caller must be declared by the module, as the contract
            // requires and as the game provider checks.
            if (input.language && std::find(m_schema.languages.begin(), m_schema.languages.end(),
                                            *input.language) == m_schema.languages.end()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this package does not declare the language " + *input.language);
            }
            const std::tuple<const std::optional<double> &, const CommonApi::Knob &, double,
                             const char *>
                knobs[] = {
                    {input.boundaryThreshold, m_schema.boundaryThreshold,
                     FALLBACK_BOUNDARY_THRESHOLD,                                                   "boundaryThreshold"},
                    {input.boundaryRadius,    m_schema.boundaryRadius,    FALLBACK_BOUNDARY_RADIUS,
                     "boundaryRadius"                                                                                  },
                    {input.noteThreshold,     m_schema.noteThreshold,     FALLBACK_NOTE_THRESHOLD,
                     "noteThreshold"                                                                                   },
            };
            for (const auto &[given, knob, fallback, what] : knobs) {
                if (auto chosen = otter::chooseKnob(given, knob, fallback, what); !chosen) {
                    return chosen.takeError();
                }
            }
            if (auto chosen =
                    otter::chooseKnob(input.steps, m_schema.steps, FALLBACK_STEPS, "steps");
                !chosen) {
                return chosen.takeError();
            }
            auto cutoff = otter::chooseKnob(input.notePresenceCutoff, m_schema.notePresenceCutoff,
                                            FALLBACK_NOTE_PRESENCE_CUTOFF, "notePresenceCutoff");
            if (!cutoff) {
                return cutoff.takeError();
            }
            if (input.progress) {
                input.progress(0);
            }
            if (!sleepUnlessStopped(m_settings.delay, [this] { return cancelled(); })) {
                return otter::cancelledError();
            }

            auto result = std::make_unique<NoteApi::NoteResult>();
            if (!input.knownNotes.empty() && !m_schema.supportsKnownNotes) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this model cannot be conditioned on known notes");
            }
            if (!input.knownNotes.empty()) {
                // The alignment path keeps the boundaries given by the caller and fills in
                // pitches. Known notes are already on the host's timeline, so they are kept
                // unchanged. The rejections are those of the game provider, through the same
                // check, so a contract test written against this stub also applies to the shipped
                // analyzer.
                if (auto checked = otter::support::checkKnownNotes(input.knownNotes, input.audio);
                    !checked) {
                    return checked.takeError();
                }
                int key = 60;
                for (const auto &known : input.knownNotes) {
                    result->notes.push_back({key, known.start, known.duration, 1.0});
                    key = key < 71 ? key + 1 : 60;
                }
            } else {
                const auto total = input.audio.duration();
                int index = 0;
                for (double at = 0; at + m_settings.beat <= total; at += m_settings.beat) {
                    // A ramp of confidences, so that the cutoff removes some notes.
                    const double confidence = 0.25 + 0.25 * (index % 4);
                    if (confidence >= *cutoff) {
                        result->notes.push_back({60 + index % 12, input.audio.startTime + at,
                                                 m_settings.beat, confidence});
                    }
                    ++index;
                }
            }
            if (input.progress) {
                input.progress(1);
            }
            return result;
        }
    };

    class StubAlignExecutive
        : public StubExecutive<AlignApi::AlignExecutive, AlignApi::AlignSchema> {
    public:
        using StubExecutive::StubExecutive;

    protected:
        srt::Expected<std::unique_ptr<AlignApi::AlignResult>>
            run(const AlignApi::AlignStartInput &input) override {
            if (auto checked = validate(input.audio); !checked) {
                return checked.takeError();
            }
            if (input.lyrics.empty()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "an alignment requires the sung text, and lyrics is empty");
            }
            // The language and scheme are chosen as the hfa provider chooses them: the declared
            // default applies to an execution that names no language, and a language declared with
            // more than one scheme requires the caller to specify the scheme.
            const AlignApi::LanguageInfo *entry = nullptr;
            const std::string language = input.language.value_or(m_schema.defaultLanguage);
            if (!language.empty() || !m_schema.languages.empty()) {
                for (const auto &candidate : m_schema.languages) {
                    if (candidate.language != language ||
                        (input.scheme && candidate.scheme != *input.scheme)) {
                        continue;
                    }
                    if (entry != nullptr) {
                        return srt::Error(srt::Error::InvalidArgument,
                                          "this package declares " + language +
                                              " with more than one scheme, so the caller must "
                                              "specify a scheme");
                    }
                    entry = &candidate;
                }
                if (entry == nullptr) {
                    return srt::Error(srt::Error::InvalidArgument,
                                      "the language " + language +
                                          " is not declared by this package");
                }
            }
            const auto &labels = input.nonSpeechPhonemes.empty() ? m_schema.defaultNonSpeechPhonemes
                                                                 : input.nonSpeechPhonemes;
            for (const auto &label : labels) {
                if (std::find(m_schema.nonSpeechPhonemes.begin(), m_schema.nonSpeechPhonemes.end(),
                              label) == m_schema.nonSpeechPhonemes.end()) {
                    return srt::Error(srt::Error::InvalidArgument,
                                      "the caller asked for the non-speech phoneme " + label +
                                          ", which this package does not declare");
                }
            }
            const std::tuple<const std::optional<double> &, const CommonApi::Knob &, double,
                             const char *>
                knobs[] = {
                    {input.nonSpeechThreshold,   m_schema.nonSpeechThreshold,
                     FALLBACK_NON_SPEECH_THRESHOLD,                                                "nonSpeechThreshold"  },
                    {input.nonSpeechMinDuration, m_schema.nonSpeechMinDuration,
                     FALLBACK_NON_SPEECH_MIN_DURATION,                                             "nonSpeechMinDuration"},
                    {input.gapFill,              m_schema.gapFill,              FALLBACK_GAP_FILL, "gapFill"             },
            };
            for (const auto &[given, knob, fallback, what] : knobs) {
                if (auto chosen = otter::chooseKnob(given, knob, fallback, what); !chosen) {
                    return chosen.takeError();
                }
            }
            if (input.progress) {
                input.progress(0);
            }
            if (!sleepUnlessStopped(m_settings.delay, [this] { return cancelled(); })) {
                return otter::cancelledError();
            }

            std::vector<std::string> written;
            std::istringstream stream(input.lyrics);
            for (std::string word; stream >> word;) {
                written.push_back(word);
            }
            auto result = std::make_unique<AlignApi::AlignResult>();
            if (entry != nullptr) {
                result->language = entry->language;
                result->scheme = entry->scheme;
            }
            // The words divide the span into equal parts, and each word consists of one phoneme
            // with the word's own text.
            const double span = input.audio.duration() / static_cast<double>(written.size());
            for (std::size_t i = 0; i < written.size(); ++i) {
                AlignApi::WordInfo word;
                word.text = written[i];
                word.start = input.audio.startTime + span * static_cast<double>(i);
                word.duration = span;
                word.phones.push_back({written[i], word.start, span});
                result->words.push_back(std::move(word));
            }
            if (input.progress) {
                input.progress(1);
            }
            return result;
        }
    };

    /// An analysis executive that implements no contract.
    class ForeignExecutive : public otter::AnalysisExecutive {
    public:
        using AnalysisExecutive::AnalysisExecutive;

        srt::ITask::State state() const noexcept override {
            return srt::ITask::Idle;
        }

        srt::Expected<void> stop() override {
            return srt::Expected<void>();
        }

        srt::Expected<void> waitForFinished() override {
            return srt::Expected<void>();
        }
    };

    /// Creates one stub analyzer from a loaded declaration. \a Implementation is the class that
    /// implements the contract whose exports are \a Schema.
    ///
    /// \return The executive; a ForeignExecutive if the configuration sets foreignExecutive;
    ///         \c AnalysisError::Internal if the spec carries no stub exports or configuration.
    template <class Schema, class Implementation>
    srt::Expected<std::unique_ptr<srt::InferenceExecutive>> createStub(srt::InferenceSpec &spec) {
        const auto schema = spec.exports() ? spec.exports()->as<Schema>() : nullptr;
        const auto configuration =
            spec.configuration() ? spec.configuration()->as<StubConfiguration>() : nullptr;
        if (schema == nullptr || configuration == nullptr) {
            return srt::Error(otter::AnalysisError::Internal,
                              "this declaration carries no stub exports or configuration");
        }
        if (configuration->settings.foreignExecutive) {
            return std::unique_ptr<srt::InferenceExecutive>(new ForeignExecutive(spec));
        }
        return std::unique_ptr<srt::InferenceExecutive>(
            new Implementation(spec, *schema, configuration->settings));
    }

    srt::Expected<StubSettings> readSettings(const srt::ContribSpec &spec) {
        StubSettings settings;
        const auto &value = spec.manifestConfiguration();
        if (value.isNull()) {
            return settings;
        }
        if (!value.isObject()) {
            return srt::Error(srt::Error::InvalidFormat,
                              "the stub configuration must be an object");
        }
        const auto object = value.toObject();
        if (auto checked = otter::manifest::rejectUnknownKeys(
                object, {"delay", "frequency", "beat", "foreignExecutive"},
                "the stub configuration");
            !checked) {
            return checked.takeError();
        }
        if (const auto it = object.find("foreignExecutive"); it != object.end()) {
            if (!it->second.isBool()) {
                return srt::Error(srt::Error::InvalidFormat, "foreignExecutive must be a boolean");
            }
            settings.foreignExecutive = it->second.toBool();
        }
        const std::pair<const char *, double StubSettings::*> numbers[] = {
            {"delay",     &StubSettings::delay    },
            {"frequency", &StubSettings::frequency},
            {"beat",      &StubSettings::beat     },
        };
        for (const auto &[key, member] : numbers) {
            const auto it = object.find(key);
            if (it == object.end()) {
                continue;
            }
            auto number = otter::manifest::readPositiveDouble(it->second, key);
            if (!number) {
                return number.takeError();
            }
            settings.*member = number.take();
        }
        return settings;
    }

    class StubF0Interpreter : public otter::AnalysisInterpreter {
    public:
        StubF0Interpreter()
            : AnalysisInterpreter(F0Api::API_INTERFACE, F0Api::API_LEVEL, VARIANT) {
        }

        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override {
            auto schema = F0Api::readF0Schema(spec, VARIANT);
            if (!schema) {
                return schema.takeError();
            }
            return std::unique_ptr<srt::ContribExports>(schema.take().release());
        }

        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override {
            auto settings = readSettings(spec);
            if (!settings) {
                return settings.takeError();
            }
            return std::unique_ptr<srt::ContribConfiguration>(
                new StubConfiguration(F0Api::API_INTERFACE, F0Api::API_LEVEL, settings.take()));
        }

        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override {
            return createStub<F0Api::F0Schema, StubF0Executive>(spec);
        }
    };

    class StubNoteInterpreter : public otter::AnalysisInterpreter {
    public:
        StubNoteInterpreter()
            : AnalysisInterpreter(NoteApi::API_INTERFACE, NoteApi::API_LEVEL, VARIANT) {
        }

        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override {
            auto schema = NoteApi::readNoteSchema(spec, VARIANT);
            if (!schema) {
                return schema.takeError();
            }
            return std::unique_ptr<srt::ContribExports>(schema.take().release());
        }

        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override {
            auto settings = readSettings(spec);
            if (!settings) {
                return settings.takeError();
            }
            return std::unique_ptr<srt::ContribConfiguration>(
                new StubConfiguration(NoteApi::API_INTERFACE, NoteApi::API_LEVEL, settings.take()));
        }

        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override {
            return createStub<NoteApi::NoteSchema, StubNoteExecutive>(spec);
        }
    };

    class StubAlignInterpreter : public otter::AnalysisInterpreter {
    public:
        StubAlignInterpreter()
            : AnalysisInterpreter(AlignApi::API_INTERFACE, AlignApi::API_LEVEL, VARIANT) {
        }

        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override {
            auto schema = AlignApi::readAlignSchema(spec, VARIANT);
            if (!schema) {
                return schema.takeError();
            }
            return std::unique_ptr<srt::ContribExports>(schema.take().release());
        }

        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override {
            auto settings = readSettings(spec);
            if (!settings) {
                return settings.takeError();
            }
            return std::unique_ptr<srt::ContribConfiguration>(new StubConfiguration(
                AlignApi::API_INTERFACE, AlignApi::API_LEVEL, settings.take()));
        }

        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override {
            return createStub<AlignApi::AlignSchema, StubAlignExecutive>(spec);
        }
    };

    class StubPlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(std::string_view interfaceName, int level, std::string_view variant) override {
            if (variant != VARIANT) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this plugin serves only the stub variant");
            }
            if (interfaceName == F0Api::API_INTERFACE && level == F0Api::API_LEVEL) {
                return std::unique_ptr<srt::ContribInterpreter>(new StubF0Interpreter());
            }
            if (interfaceName == NoteApi::API_INTERFACE && level == NoteApi::API_LEVEL) {
                return std::unique_ptr<srt::ContribInterpreter>(new StubNoteInterpreter());
            }
            if (interfaceName == AlignApi::API_INTERFACE && level == AlignApi::API_LEVEL) {
                return std::unique_ptr<srt::ContribInterpreter>(new StubAlignInterpreter());
            }
            return srt::Error(srt::Error::FeatureNotSupported,
                              "this plugin does not serve " + std::string(interfaceName) +
                                  " level " + std::to_string(level));
        }
    };

}

STDC_EXPORT_PLUGIN(StubPlugin)
