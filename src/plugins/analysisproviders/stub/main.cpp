// A provider that answers both Level 1 contracts without a model of any kind.
//
// It exists so that the parts of otter that have nothing to do with inference, such as
// registering the category, loading a package, discovering the interpreter, attaching the
// extension, creating and cancelling an executive, can be exercised on their own. A failure here
// is a framework failure; a failure in the shipped providers with this one passing is a model or a
// driver failure. Keeping the two apart is the whole point.
//
// Its answers are arithmetic, not analysis: a tone at a fixed frequency for F0, one note per
// declared beat for Note. They are deterministic, which is what a test wants.
//
// It reads its declaration the way a shipped provider does: the audio format and the knobs come
// from exports through the library's readers, and only its own three settings come from
// configuration. A stub that were laxer than the real providers would let a contract test pass
// while describing behaviour no shipped analyzer has, which is worse than having no stub.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/Support/Expected.h>
#include <synthrt/Support/JSON.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/AnalysisInput.h>
#include <otter/Analysis/AnalysisProvider.h>
#include <otter/Analysis/AnalysisProviderPlugin.h>
#include <otter/Api/F0/1/F0ApiL1.h>
#include <otter/Api/Note/1/NoteApiL1.h>
#include <otter/Support/ManifestValues.h>

namespace F0Api = otter::Api::F0::L1;
namespace NoteApi = otter::Api::Note::L1;
namespace CommonApi = otter::Api::Common::L1;

namespace {

    constexpr char VARIANT[] = "stub";

    /// Blocks in small steps so a test can observe a running execution and cancel it.
    ///
    /// The delay is declared per contribution and defaults to none, so a test that only wants a
    /// result does not pay for one.
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
    };

    /// The stub's configuration block, kept on the spec like any variant's.
    class StubConfiguration : public srt::ContribConfiguration {
    public:
        StubConfiguration(const char *interfaceName, int level, StubSettings settings)
            : srt::ContribConfiguration(interfaceName, VARIANT, level), settings(settings) {
        }

        StubSettings settings;
    };

    /// What both stub executives share: the schema the audio format and knobs come from, the
    /// settings, and the one validation call. The lifecycle is the contract's own.
    template <class Contract, class Schema>
    class StubExecutive : public Contract {
    public:
        StubExecutive(otter::AnalysisSpec &spec, const Schema &schema, StubSettings settings)
            : Contract(spec), m_schema(schema), m_settings(settings) {
        }

        ~StubExecutive() {
            (void) this->stop();
            (void) this->waitForFinished();
        }

    protected:
        /// Checks exactly what a shipped provider checks, through the same library call.
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
                                                0.03, "voicingThreshold");
                !chosen) {
                return chosen.takeError();
            }
            if (input.progress) {
                input.progress(0);
            }
            if (!sleepUnlessStopped(m_settings.delay, [this] { return cancelled(); })) {
                return srt::Error(otter::AnalysisError::Cancelled, "the execution was cancelled");
            }

            const auto frames =
                static_cast<std::size_t>(input.audio.duration() / m_schema.interval);
            auto result = std::make_unique<F0Api::F0Result>();
            result->startTime = input.audio.startTime;
            result->interval = m_schema.interval;
            result->f0.resize(frames);
            result->voiced.resize(frames);
            // Voiced for the first half of every second, unvoiced for the rest, so that a test
            // sees both kinds of frame and can tell interpolation on from off.
            for (std::size_t i = 0; i < frames; ++i) {
                const bool voiced = (i % 100) < 50;
                result->voiced[i] = voiced ? 1 : 0;
                result->f0[i] = voiced ? static_cast<float>(m_settings.frequency) : 0.0f;
            }
            if (otter::chooseKnob(input.interpolateUnvoiced, m_schema.interpolateUnvoiced, true)) {
                for (std::size_t i = 0; i < frames; ++i) {
                    if (!result->voiced[i]) {
                        result->f0[i] = static_cast<float>(m_settings.frequency);
                    }
                }
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
            // A module that lists languages knows only those. One that lists none does not
            // distinguish, and takes whatever it is given.
            if (input.language && !m_schema.languages.empty() &&
                std::find(m_schema.languages.begin(), m_schema.languages.end(), *input.language) ==
                    m_schema.languages.end()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this model does not know the language " + *input.language);
            }
            const std::pair<const std::optional<double> &, const CommonApi::Knob &> knobs[] = {
                {input.boundaryThreshold, m_schema.boundaryThreshold},
                {input.boundaryRadius,    m_schema.boundaryRadius   },
                {input.noteThreshold,     m_schema.noteThreshold    },
            };
            for (const auto &[given, knob] : knobs) {
                if (auto chosen = otter::chooseKnob(given, knob, 0.2, "knob"); !chosen) {
                    return chosen.takeError();
                }
            }
            if (auto chosen = otter::chooseKnob(input.steps, m_schema.steps, 8, "steps"); !chosen) {
                return chosen.takeError();
            }
            auto cutoff = otter::chooseKnob(input.notePresenceCutoff, m_schema.notePresenceCutoff,
                                            0.0, "notePresenceCutoff");
            if (!cutoff) {
                return cutoff.takeError();
            }
            if (input.progress) {
                input.progress(0);
            }
            if (!sleepUnlessStopped(m_settings.delay, [this] { return cancelled(); })) {
                return srt::Error(otter::AnalysisError::Cancelled, "the execution was cancelled");
            }

            auto result = std::make_unique<NoteApi::NoteResult>();
            if (!input.knownNotes.empty() && !m_schema.supportsKnownNotes) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this model cannot be conditioned on known notes");
            }
            if (!input.knownNotes.empty()) {
                // The alignment path: keep the boundaries the caller gave and fill in pitches.
                // Known notes are already on the host's timeline, so they are kept as they are.
                // The same three refusals the game provider makes, so a contract test written
                // against this stub says something about the shipped analyzer too.
                int key = 60;
                double previousEnd = input.audio.startTime;
                for (const auto &known : input.knownNotes) {
                    if (known.duration <= 0) {
                        return srt::Error(srt::Error::InvalidArgument,
                                          "a known note has no duration");
                    }
                    if (known.start < input.audio.startTime - 1e-9) {
                        return srt::Error(srt::Error::InvalidArgument,
                                          "a known note starts before the audio does");
                    }
                    if (known.start < previousEnd - 1e-9) {
                        return srt::Error(srt::Error::InvalidArgument,
                                          "the known notes overlap or are out of order");
                    }
                    result->notes.push_back({key, known.start, known.duration, 1.0});
                    key = key < 71 ? key + 1 : 60;
                    previousEnd = known.start + known.duration;
                }
            } else {
                const auto total = input.audio.duration();
                int index = 0;
                for (double at = 0; at + m_settings.beat <= total; at += m_settings.beat) {
                    // A ramp of confidences so that the cutoff has something to cut.
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

    /// Hands out one stub analyzer. \a Executive names the contract, \a Implementation the
    /// class that answers it; the contract is what the extension ID is keyed on.
    template <class Executive, class Schema, class Implementation>
    class StubExtension : public otter::AnalysisExtension {
    public:
        StubExtension(otter::AnalysisSpec &spec, StubSettings settings)
            : AnalysisExtension(
                  spec, srt::ContribSpecExtensionTraits<otter::AnalysisSpec, Executive>::ID),
              m_settings(settings) {
        }

        srt::Expected<std::unique_ptr<otter::AnalysisExecutive>>
            createAnalyzer(const otter::AnalysisRuntimeOptions &runtimeOptions) override {
            if (auto checked = otter::checkRuntimeOptions(runtimeOptions, spec()); !checked) {
                return checked.takeError();
            }
            const auto schema = spec().exports() ? spec().exports()->as<Schema>() : nullptr;
            if (schema == nullptr) {
                return srt::Error(srt::Error::InvalidFormat, "this declaration carries no exports");
            }
            return std::unique_ptr<otter::AnalysisExecutive>(
                new Implementation(spec(), *schema, m_settings));
        }

    private:
        StubSettings m_settings;
    };

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
                object, {"delay", "frequency", "beat"}, "the stub configuration");
            !checked) {
            return checked.takeError();
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

    class StubF0Provider : public otter::AnalysisProvider {
    public:
        StubF0Provider() : AnalysisProvider(F0Api::API_INTERFACE, F0Api::API_LEVEL, VARIANT) {
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

    protected:
        srt::Expected<std::unique_ptr<otter::AnalysisExtension>>
            createAnalysisExtension(otter::AnalysisSpec &spec) const override {
            auto settings = readSettings(spec);
            if (!settings) {
                return settings.takeError();
            }
            return std::unique_ptr<otter::AnalysisExtension>(
                new StubExtension<F0Api::F0Executive, F0Api::F0Schema, StubF0Executive>(
                    spec, settings.take()));
        }
    };

    class StubNoteProvider : public otter::AnalysisProvider {
    public:
        StubNoteProvider() : AnalysisProvider(NoteApi::API_INTERFACE, NoteApi::API_LEVEL, VARIANT) {
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

    protected:
        srt::Expected<std::unique_ptr<otter::AnalysisExtension>>
            createAnalysisExtension(otter::AnalysisSpec &spec) const override {
            auto settings = readSettings(spec);
            if (!settings) {
                return settings.takeError();
            }
            return std::unique_ptr<otter::AnalysisExtension>(
                new StubExtension<NoteApi::NoteExecutive, NoteApi::NoteSchema, StubNoteExecutive>(
                    spec, settings.take()));
        }
    };

    class StubProviderPlugin : public otter::AnalysisProviderPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(std::string_view interfaceName, int level, std::string_view variant) override {
            if (variant != VARIANT) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this plugin serves only the stub variant");
            }
            if (interfaceName == F0Api::API_INTERFACE && level == F0Api::API_LEVEL) {
                return std::unique_ptr<srt::ContribInterpreter>(new StubF0Provider());
            }
            if (interfaceName == NoteApi::API_INTERFACE && level == NoteApi::API_LEVEL) {
                return std::unique_ptr<srt::ContribInterpreter>(new StubNoteProvider());
            }
            return srt::Error(srt::Error::FeatureNotSupported,
                              "this plugin does not serve " + std::string(interfaceName) +
                                  " level " + std::to_string(level));
        }
    };

}

STDC_EXPORT_PLUGIN(StubProviderPlugin)
