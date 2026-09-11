// A provider that answers both Level 1 contracts without a model of any kind.
//
// It exists so that the parts of otter that have nothing to do with inference — registering the
// category, loading a package, discovering the interpreter, attaching the extension, creating and
// cancelling an executive — can be exercised on their own. A failure here is a framework failure;
// a failure in the shipped providers with this one passing is a model or a driver failure. Keeping
// the two apart is the whole point.
//
// Its answers are arithmetic, not analysis: a tone at a fixed frequency for F0, one note per
// declared beat for Note. They are deterministic, which is what a test wants.

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/Support/Expected.h>
#include <synthrt/Support/JSON.h>

#include <otter/Analysis/AnalysisProvider.h>
#include <otter/Analysis/AnalysisRunner.h>
#include <otter/Analysis/AnalysisProviderPlugin.h>
#include <otter/Api/F0/1/F0ApiL1.h>
#include <otter/Api/Note/1/NoteApiL1.h>
#include <otter/Support/ManifestValues.h>

namespace F0Api = otter::Api::F0::L1;
namespace NoteApi = otter::Api::Note::L1;
namespace CommonApi = otter::Api::Common::L1;

namespace {

    constexpr char VARIANT[] = "stub";
    constexpr int SAMPLE_RATE = 16000;
    constexpr double INTERVAL = 0.01;
    constexpr double MAX_SEGMENT = 60.0;

    /// Blocks in small steps so a test can observe a running execution and cancel it.
    ///
    /// The delay is declared per contribution and defaults to none, so a test that only wants a
    /// result does not pay for one.
    bool sleepUnlessStopped(double seconds, const otter::AnalysisRunner &runner) {
        const auto steps = static_cast<int>(seconds * 200);
        for (int i = 0; i < steps; ++i) {
            if (runner.cancelled()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return !runner.cancelled();
    }

    /// The knobs and delays a stub declaration may carry.
    struct StubSettings {
        double delay = 0;
        double frequency = 440;
        double beat = 0.5;
        bool supportsKnownNotes = true;
    };

    /// Shared lifecycle for both stub executives.
    template <class Contract>
    class StubExecutive : public Contract {
    public:
        StubExecutive(otter::AnalysisSpec &spec, StubSettings settings)
            : Contract(spec), m_settings(settings) {
        }

        ~StubExecutive() override {
            m_runner.cancel();
            m_runner.wait();
        }

        srt::ITask::State state() const noexcept override {
            return m_runner.state();
        }

        srt::Expected<void> stop() override {
            m_runner.cancel();
            return srt::Expected<void>();
        }

        srt::Expected<void> waitForFinished() override {
            m_runner.wait();
            return srt::Expected<void>();
        }

    protected:
        /// Claims the execution on the caller's thread, then runs \a body on a worker.
        template <class Input, class Callback, class Body>
        srt::Expected<void> spawn(std::shared_ptr<const Input> input, Callback callback,
                                  Body body) {
            if (!input) {
                return srt::Error(srt::Error::InvalidArgument, "no input was supplied");
            }
            if (!m_runner.begin()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this analyzer is already running an execution");
            }
            auto spawned = m_runner.spawn(
                [this, input, callback = std::move(callback), body = std::move(body)]() mutable {
                    auto result = body(*input);
                    m_runner.end(static_cast<bool>(result));
                    if (callback) {
                        callback(std::move(result));
                    }
                });
            if (!spawned) {
                m_runner.end(false);
            }
            return spawned;
        }

        /// Checks what every contract checks, so the two stubs agree on what a bad input is.
        srt::Expected<void> validate(const CommonApi::AudioSegment &audio) const {
            if (audio.sampleRate != SAMPLE_RATE) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this model needs " + std::to_string(SAMPLE_RATE) +
                                      " Hz and was given " + std::to_string(audio.sampleRate) +
                                      " Hz");
            }
            if (audio.channelCount < 1 || audio.samples.empty()) {
                return srt::Error(srt::Error::InvalidArgument, "the audio holds no samples");
            }
            if (audio.duration() > MAX_SEGMENT) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this model accepts at most " + std::to_string(MAX_SEGMENT) +
                                      " seconds in one execution");
            }
            return srt::Expected<void>();
        }

        StubSettings m_settings;
        otter::AnalysisRunner m_runner;
    };

    class StubF0Executive : public StubExecutive<F0Api::F0Executive> {
    public:
        using StubExecutive::StubExecutive;

        srt::Expected<std::unique_ptr<F0Api::F0Result>>
            start(const F0Api::F0StartInput &input) override {
            if (!m_runner.begin()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this analyzer is already running an execution");
            }
            auto result = run(input);
            m_runner.end(static_cast<bool>(result));
            return result;
        }

        srt::Expected<std::unique_ptr<F0Api::F0Result>> run(const F0Api::F0StartInput &input) {
            if (auto checked = validate(input.audio); !checked) {
                return checked.takeError();
            }
            if (input.progress) {
                input.progress(0);
            }
            if (!sleepUnlessStopped(m_settings.delay, m_runner)) {
                return srt::Error(srt::Error::InvalidArgument, "the execution was cancelled");
            }

            const auto frames =
                static_cast<std::size_t>(input.audio.duration() / INTERVAL);
            auto result = std::make_unique<F0Api::F0Result>();
            result->startTime = input.audio.startTime;
            result->interval = INTERVAL;
            result->f0.resize(frames);
            result->voiced.resize(frames);
            // Voiced for the first half of every second, unvoiced for the rest, so that a test
            // sees both kinds of frame and can tell interpolation on from off.
            for (std::size_t i = 0; i < frames; ++i) {
                const bool voiced = (i % 100) < 50;
                result->voiced[i] = voiced ? 1 : 0;
                result->f0[i] = voiced ? static_cast<float>(m_settings.frequency) : 0.0f;
            }
            if (input.interpolateUnvoiced.value_or(true)) {
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

        srt::Expected<void> startAsync(std::shared_ptr<const F0Api::F0StartInput> input,
                                       AsyncCallback callback) override {
            return spawn<F0Api::F0StartInput>(
                std::move(input), std::move(callback),
                [this](const F0Api::F0StartInput &one) { return run(one); });
        }
    };

    class StubNoteExecutive : public StubExecutive<NoteApi::NoteExecutive> {
    public:
        using StubExecutive::StubExecutive;

        srt::Expected<std::unique_ptr<NoteApi::NoteResult>>
            start(const NoteApi::NoteStartInput &input) override {
            if (!m_runner.begin()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this analyzer is already running an execution");
            }
            auto result = run(input);
            m_runner.end(static_cast<bool>(result));
            return result;
        }

        srt::Expected<std::unique_ptr<NoteApi::NoteResult>> run(const NoteApi::NoteStartInput &input) {
            if (auto checked = validate(input.audio); !checked) {
                return checked.takeError();
            }
            if (input.language && *input.language != "zxx") {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this model does not know the language " + *input.language);
            }
            if (input.progress) {
                input.progress(0);
            }
            if (!sleepUnlessStopped(m_settings.delay, m_runner)) {
                return srt::Error(srt::Error::InvalidArgument, "the execution was cancelled");
            }

            auto result = std::make_unique<NoteApi::NoteResult>();
            const auto cutoff = input.notePresenceCutoff.value_or(0.0);
            if (!input.knownNotes.empty()) {
                // The alignment path: keep the boundaries the caller gave and fill in pitches.
                int key = 60;
                for (const auto &known : input.knownNotes) {
                    result->notes.push_back({key, input.audio.startTime + known.start,
                                             known.duration, 1.0});
                    key = key < 71 ? key + 1 : 60;
                }
            } else {
                const auto total = input.audio.duration();
                int index = 0;
                for (double at = 0; at + m_settings.beat <= total; at += m_settings.beat) {
                    // A ramp of confidences so that the cutoff has something to cut.
                    const double confidence = 0.25 + 0.25 * (index % 4);
                    if (confidence >= cutoff) {
                        result->notes.push_back({60 + index % 12,
                                                 input.audio.startTime + at, m_settings.beat,
                                                 confidence});
                    }
                    ++index;
                }
            }
            if (input.progress) {
                input.progress(1);
            }
            return result;
        }

        srt::Expected<void> startAsync(std::shared_ptr<const NoteApi::NoteStartInput> input,
                                       AsyncCallback callback) override {
            return spawn<NoteApi::NoteStartInput>(
                std::move(input), std::move(callback),
                [this](const NoteApi::NoteStartInput &one) { return run(one); });
        }
    };

    /// Hands out one stub analyzer. \a Executive names the contract, \a Implementation the
    /// class that answers it; the contract is what the extension ID is keyed on.
    template <class Executive, class Implementation>
    class StubExtension : public otter::AnalysisExtension {
    public:
        StubExtension(otter::AnalysisSpec &spec, StubSettings settings)
            : AnalysisExtension(
                  spec, srt::ContribSpecExtensionTraits<otter::AnalysisSpec, Executive>::ID),
              m_settings(settings) {
        }

        srt::Expected<std::unique_ptr<otter::AnalysisExecutive>>
            createAnalyzer(const otter::AnalysisRuntimeOptions &runtimeOptions) override {
            return std::unique_ptr<otter::AnalysisExecutive>(
                new Implementation(spec(), m_settings));
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
                object, {"delay", "frequency", "beat", "supportsKnownNotes"},
                "the stub configuration");
            !checked) {
            return checked.takeError();
        }
        if (const auto it = object.find("delay"); it != object.end()) {
            auto number = otter::manifest::readPositiveDouble(it->second, "delay");
            if (!number) {
                return number.takeError();
            }
            settings.delay = number.take();
        }
        if (const auto it = object.find("frequency"); it != object.end()) {
            auto number = otter::manifest::readPositiveDouble(it->second, "frequency");
            if (!number) {
                return number.takeError();
            }
            settings.frequency = number.take();
        }
        if (const auto it = object.find("beat"); it != object.end()) {
            auto number = otter::manifest::readPositiveDouble(it->second, "beat");
            if (!number) {
                return number.takeError();
            }
            settings.beat = number.take();
        }
        if (const auto it = object.find("supportsKnownNotes"); it != object.end()) {
            if (!it->second.isBool()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "supportsKnownNotes must be a boolean");
            }
            settings.supportsKnownNotes = it->second.toBool();
        }
        return settings;
    }

    class StubF0Provider : public otter::AnalysisProvider {
    public:
        StubF0Provider() : AnalysisProvider(F0Api::API_INTERFACE, F0Api::API_LEVEL, VARIANT) {
        }

        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override {
            auto settings = readSettings(spec);
            if (!settings) {
                return settings.takeError();
            }
            const auto values = settings.take();
            auto result = std::make_unique<F0Api::F0Schema>(VARIANT);
            result->sampleRate = SAMPLE_RATE;
            result->channelCount = 1;
            result->interval = INTERVAL;
            result->maxSegmentDuration = MAX_SEGMENT;
            result->voicingThreshold = {true, 0.0, 1.0, 0.03};
            result->interpolateUnvoiced = {true, true};
            return result;
        }

        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override {
            if (auto settings = readSettings(spec); !settings) {
                return settings.takeError();
            }
            return std::unique_ptr<srt::ContribConfiguration>(
                new F0Api::F0Configuration(VARIANT));
        }

    protected:
        srt::Expected<std::unique_ptr<otter::AnalysisExtension>>
            createAnalysisExtension(otter::AnalysisSpec &spec) const override {
            auto settings = readSettings(spec);
            if (!settings) {
                return settings.takeError();
            }
            return std::unique_ptr<otter::AnalysisExtension>(
                new StubExtension<F0Api::F0Executive, StubF0Executive>(spec, settings.take()));
        }
    };

    class StubNoteProvider : public otter::AnalysisProvider {
    public:
        StubNoteProvider() : AnalysisProvider(NoteApi::API_INTERFACE, NoteApi::API_LEVEL, VARIANT) {
        }

        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override {
            auto settings = readSettings(spec);
            if (!settings) {
                return settings.takeError();
            }
            const auto values = settings.take();
            auto result = std::make_unique<NoteApi::NoteSchema>(VARIANT);
            result->sampleRate = SAMPLE_RATE;
            result->channelCount = 1;
            result->maxSegmentDuration = MAX_SEGMENT;
            result->languages = {"zxx"};
            result->supportsKnownNotes = values.supportsKnownNotes;
            result->boundaryThreshold = {true, 0.0, 1.0, 0.2};
            result->boundaryRadius = {true, 0.0, 1.0, 0.02};
            result->noteThreshold = {true, 0.0, 1.0, 0.2};
            result->notePresenceCutoff = {true, 0.0, 1.0, 0.5};
            result->steps = {true, 1, 64, 8};
            return result;
        }

        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override {
            if (auto settings = readSettings(spec); !settings) {
                return settings.takeError();
            }
            return std::unique_ptr<srt::ContribConfiguration>(
                new NoteApi::NoteConfiguration(VARIANT));
        }

    protected:
        srt::Expected<std::unique_ptr<otter::AnalysisExtension>>
            createAnalysisExtension(otter::AnalysisSpec &spec) const override {
            auto settings = readSettings(spec);
            if (!settings) {
                return settings.takeError();
            }
            return std::unique_ptr<otter::AnalysisExtension>(
                new StubExtension<NoteApi::NoteExecutive, StubNoteExecutive>(spec, settings.take()));
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
