#include <atomic>
#include <cmath>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#include <stdcorelib/path.h>
#include <stdcorelib/plugin/plugin.h>

#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/Support/Expected.h>
#include <synthrt/Support/JSON.h>

#include <dsinfer/Api/Drivers/Onnx/OnnxDriverApi.h>
#include <dsinfer/Core/Tensor.h>
#include <dsinfer/Inference/InferenceDriver.h>
#include <dsinfer/Inference/InferenceDriverPlugin.h>
#include <dsinfer/Inference/InferenceSession.h>

#include <otter/Analysis/AnalysisProvider.h>
#include <otter/Analysis/AnalysisRunner.h>
#include <otter/Analysis/AnalysisProviderPlugin.h>
#include <otter/Api/F0/1/F0ApiL1.h>

#include <otter/Support/ManifestValues.h>

namespace F0Api = otter::Api::F0::L1;
namespace CommonApi = otter::Api::Common::L1;
namespace OnnxApi = ds::Api::Onnx;

namespace {

    /// The variant this provider implements.
    constexpr char VARIANT[] = "rmvpe";

    /// The backend whose driver this variant needs.
    constexpr char BACKEND[] = "onnx";

    /// Interpolates the curve across unvoiced frames, in the log domain.
    ///
    /// \a voiced marks the frames that carry a measurement; the rest are filled from their
    /// neighbours, and the ends are held flat. An all-unvoiced span has nothing to anchor on and
    /// is left alone.
    ///
    /// The implementation this is derived from read its mask the other way round from how the mask
    /// was documented, and only one of the two readings makes the arithmetic mean anything: an
    /// unvoiced frame carries no pitch, so it is the one that must be filled in. Stated here
    /// because the field name alone did not settle it.
    void interpolateUnvoiced(std::vector<float> &f0, const std::vector<uint8_t> &voiced) {
        const auto n = static_cast<int>(f0.size());
        int firstVoiced = -1;
        int lastVoiced = -1;
        for (int i = 0; i < n; ++i) {
            if (voiced[i]) {
                firstVoiced = i;
                break;
            }
        }
        if (firstVoiced < 0) {
            return;
        }
        for (int i = n - 1; i >= 0; --i) {
            if (voiced[i]) {
                lastVoiced = i;
                break;
            }
        }

        for (int i = 0; i < firstVoiced; ++i) {
            f0[i] = f0[firstVoiced];
        }
        for (int i = n - 1; i > lastVoiced; --i) {
            f0[i] = f0[lastVoiced];
        }
        for (int i = firstVoiced; i < lastVoiced; ++i) {
            if (voiced[i]) {
                continue;
            }
            const int prev = i - 1;
            int next = i + 1;
            while (next < n && !voiced[next]) {
                ++next;
            }
            if (next >= n || f0[prev] <= 0 || f0[next] <= 0) {
                continue;
            }
            const auto ratio = std::log(f0[next] / f0[prev]);
            f0[i] = static_cast<float>(f0[prev] * std::exp(ratio * static_cast<double>(i - prev) /
                                                           (next - prev)));
        }
    }

    /// Averages interleaved channels down to one.
    ///
    /// Not an audio module: a model that wants mono and is handed two channels would otherwise
    /// have to refuse, and averaging is the whole of what the refusal would be asking the caller
    /// to do. Resampling is a different matter and stays with the host.
    std::vector<float> downmix(const std::vector<float> &samples, int channelCount) {
        if (channelCount <= 1) {
            return samples;
        }
        const auto frames = samples.size() / static_cast<std::size_t>(channelCount);
        std::vector<float> mono(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            float sum = 0;
            for (int c = 0; c < channelCount; ++c) {
                sum += samples[i * channelCount + c];
            }
            mono[i] = sum / static_cast<float>(channelCount);
        }
        return mono;
    }

    /// Runs one RMVPE model.
    class RmvpeExecutive : public F0Api::F0Executive {
    public:
        RmvpeExecutive(otter::AnalysisSpec &spec, std::unique_ptr<ds::InferenceSession> session,
                       const F0Api::F0Configuration &configuration)
            : F0Executive(spec), m_session(std::move(session)),
              m_sampleRate(configuration.sampleRate), m_channelCount(configuration.channelCount),
              m_interval(configuration.interval),
              m_maxSegmentDuration(configuration.maxSegmentDuration),
              m_defaultVoicingThreshold(configuration.defaultVoicingThreshold),
              m_defaultInterpolate(configuration.defaultInterpolateUnvoiced) {
        }

        ~RmvpeExecutive() override {
            // The runner's destructor cancels and waits, but it runs after this body, and the
            // session must not be torn down under a forward pass still in flight.
            m_runner.cancel();
            m_runner.wait();
            if (m_session) {
                m_session->stop();
                m_session->close();
            }
        }

        srt::ITask::State state() const noexcept override {
            return m_runner.state();
        }

        srt::Expected<void> stop() override {
            m_runner.cancel();
            // The session is what is actually blocking, so it hears about it too. A model already
            // inside a forward pass is not interruptible everywhere, which is why the runner's
            // flag is checked around the call as well.
            if (m_session) {
                return m_session->stop();
            }
            return srt::Expected<void>();
        }

        srt::Expected<void> waitForFinished() override {
            m_runner.wait();
            if (m_session) {
                return m_session->waitForFinished();
            }
            return srt::Expected<void>();
        }

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

        srt::Expected<void> startAsync(std::shared_ptr<const F0Api::F0StartInput> input,
                                       AsyncCallback callback) override {
            if (!input) {
                return srt::Error(srt::Error::InvalidArgument, "no input was supplied");
            }
            // Claimed here, on the caller's thread, so that a stop arriving the instant this
            // returns lands on this execution rather than being cleared by the worker.
            if (!m_runner.begin()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this analyzer is already running an execution");
            }
            auto spawned = m_runner.spawn([this, input, callback = std::move(callback)]() mutable {
                auto result = run(*input);
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

    private:
        srt::Expected<std::unique_ptr<F0Api::F0Result>> run(const F0Api::F0StartInput &input) {
            const auto &audio = input.audio;
            if (audio.sampleRate != m_sampleRate) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this model needs " + std::to_string(m_sampleRate) +
                                      " Hz and was given " + std::to_string(audio.sampleRate) +
                                      " Hz; the host resamples, this analyzer does not");
            }
            if (audio.channelCount < 1) {
                return srt::Error(srt::Error::InvalidArgument, "the audio declares no channels");
            }
            if (audio.samples.empty()) {
                return srt::Error(srt::Error::InvalidArgument, "the audio holds no samples");
            }
            if (m_maxSegmentDuration > 0 && audio.duration() > m_maxSegmentDuration) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this model accepts at most " +
                                      std::to_string(m_maxSegmentDuration) +
                                      " seconds in one execution");
            }

            const auto waveform = downmix(audio.samples, audio.channelCount);
            const auto threshold =
                static_cast<float>(input.voicingThreshold.value_or(m_defaultVoicingThreshold));

            if (input.progress) {
                input.progress(0);
            }
            if (m_runner.cancelled()) {
                return srt::Error(srt::Error::InvalidArgument, "the execution was cancelled");
            }

            OnnxApi::SessionStartInput request;
            {
                auto tensor = ds::Tensor::createFromView<float>(
                    {1, static_cast<std::int64_t>(waveform.size())},
                    stdc::array_view<float>{waveform});
                if (!tensor) {
                    return tensor.takeError();
                }
                request.inputs["waveform"] = tensor.take();
            }
            {
                auto tensor = ds::Tensor::createScalar<float>(threshold);
                if (!tensor) {
                    return tensor.takeError();
                }
                request.inputs["threshold"] = tensor.take();
            }
            request.outputs = {"f0", "uv"};

            auto response = m_session->start(request);
            if (!response) {
                if (m_runner.cancelled()) {
                    return srt::Error(srt::Error::InvalidArgument,
                                      "the execution was cancelled");
                }
                return response.takeError().withContext("the rmvpe model failed");
            }
            auto taskResult = response.take();
            const auto *outputs = taskResult ? taskResult->as<OnnxApi::SessionResult>() : nullptr;
            if (outputs == nullptr) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the rmvpe model returned no outputs");
            }

            auto result = std::make_unique<F0Api::F0Result>();
            result->startTime = audio.startTime;
            result->interval = m_interval;

            const auto f0It = outputs->outputs.find("f0");
            if (f0It == outputs->outputs.end() || !f0It->second) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the rmvpe model did not return f0");
            }
            if (f0It->second->dataType() != ds::ITensor::Float) {
                return srt::Error(srt::Error::InvalidFormat, "f0 is not floating point");
            }
            const auto f0View = f0It->second->view<float>();
            result->f0.assign(f0View.begin(), f0View.end());

            const auto uvIt = outputs->outputs.find("uv");
            if (uvIt == outputs->outputs.end() || !uvIt->second) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the rmvpe model did not return uv");
            }
            if (uvIt->second->dataType() != ds::ITensor::Bool) {
                return srt::Error(srt::Error::InvalidFormat, "uv is not boolean");
            }
            const auto uvRaw = uvIt->second->rawView();
            if (uvRaw.size() != result->f0.size()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the rmvpe model returned " + std::to_string(uvRaw.size()) +
                                      " voicing flags for " + std::to_string(result->f0.size()) +
                                      " frames");
            }
            // The model's flag marks the frames that carry no measurement, so it is inverted to
            // reach the contract's, which marks the frames that do.
            result->voiced.resize(uvRaw.size());
            for (std::size_t i = 0; i < uvRaw.size(); ++i) {
                result->voiced[i] = uvRaw[i] == std::byte{0} ? 1 : 0;
            }

            if (input.interpolateUnvoiced.value_or(m_defaultInterpolate)) {
                interpolateUnvoiced(result->f0, result->voiced);
            } else {
                for (std::size_t i = 0; i < result->f0.size(); ++i) {
                    if (!result->voiced[i]) {
                        result->f0[i] = 0;
                    }
                }
            }

            if (input.progress) {
                input.progress(1);
            }
            return result;
        }

        std::unique_ptr<ds::InferenceSession> m_session;
        int m_sampleRate;
        int m_channelCount;
        double m_interval;
        double m_maxSegmentDuration;
        double m_defaultVoicingThreshold;
        bool m_defaultInterpolate;

        otter::AnalysisRunner m_runner;
    };

    /// Opens the model on demand and hands out analyzers.
    class RmvpeExtension : public otter::AnalysisExtension {
    public:
        explicit RmvpeExtension(otter::AnalysisSpec &spec)
            : AnalysisExtension(
                  spec, srt::ContribSpecExtensionTraits<otter::AnalysisSpec,
                                                        F0Api::F0Executive>::ID) {
        }

        srt::Expected<std::unique_ptr<otter::AnalysisExecutive>>
            createAnalyzer(const otter::AnalysisRuntimeOptions &runtimeOptions) override {
            const auto *configuration =
                spec().configuration() ? spec().configuration()->as<F0Api::F0Configuration>()
                                       : nullptr;
            if (configuration == nullptr) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "this declaration carries no rmvpe configuration");
            }

            auto *service = spec().package().synthUnit().runtimeService(
                ds::InferenceDriverPlugin::IID, BACKEND);
            if (service == nullptr) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "the onnx inference driver is not registered on this unit");
            }
            auto *driver = service->as<ds::InferenceDriver>();
            if (driver == nullptr) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "the service registered as the onnx driver is not one");
            }

            auto session = driver->createSession();
            if (!session) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "the driver created no session");
            }
            OnnxApi::SessionOpenArgs args;
            if (auto opened = session->open(configuration->model, args); !opened) {
                return opened.takeError().withContext(
                    "cannot open the rmvpe model " +
                    stdc::path::to_utf8(configuration->model));
            }
            return std::unique_ptr<otter::AnalysisExecutive>(
                new RmvpeExecutive(spec(), std::move(session), *configuration));
        }
    };

    class RmvpeProvider : public otter::AnalysisProvider {
    public:
        RmvpeProvider() : AnalysisProvider(F0Api::API_INTERFACE, F0Api::API_LEVEL, VARIANT) {
        }

        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override {
            auto result = std::make_unique<F0Api::F0Schema>(VARIANT);
            auto configuration = readConfiguration(spec);
            if (!configuration) {
                return configuration.takeError();
            }
            const auto values = configuration.take();
            result->sampleRate = values->sampleRate;
            result->channelCount = values->channelCount;
            result->interval = values->interval;
            result->maxSegmentDuration = values->maxSegmentDuration;
            result->voicingThreshold = {true, 0.0, 1.0, values->defaultVoicingThreshold};
            result->interpolateUnvoiced = {true, values->defaultInterpolateUnvoiced};

            // exports says what the module can do, and for this variant every one of those facts
            // is already in configuration. Reading it here rather than asking a declaration to
            // repeat itself keeps the two from drifting apart, which is the only way a host could
            // prepare audio in a format the model then refuses.
            if (!spec.manifestExports().isNull() && !spec.manifestExports().isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the rmvpe exports must be an object");
            }
            if (spec.manifestExports().isObject() && !spec.manifestExports().toObject().empty()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the rmvpe variant derives its exports from its configuration; "
                                  "declaring them again would let the two disagree");
            }
            return result;
        }

        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override {
            auto result = readConfiguration(spec);
            if (!result) {
                return result.takeError();
            }
            return std::unique_ptr<srt::ContribConfiguration>(result.take().release());
        }

    protected:
        srt::Expected<std::unique_ptr<otter::AnalysisExtension>>
            createAnalysisExtension(otter::AnalysisSpec &spec) const override {
            return std::unique_ptr<otter::AnalysisExtension>(new RmvpeExtension(spec));
        }

    private:
        static srt::Expected<std::unique_ptr<F0Api::F0Configuration>>
            readConfiguration(const srt::ContribSpec &spec) {
            const auto &value = spec.manifestConfiguration();
            if (!value.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the rmvpe configuration must be an object");
            }
            auto result = std::make_unique<F0Api::F0Configuration>(VARIANT);
            result->sampleRate = 16000;
            result->channelCount = 1;
            result->interval = 0.01;
            result->defaultVoicingThreshold = 0.03;
            result->defaultInterpolateUnvoiced = true;

            const auto directory = spec.declarationPath().parent_path();
            bool sawModel = false;
            for (const auto &[key, item] : value.toObject()) {
                if (key == "model") {
                    auto path = otter::manifest::readPath(item, directory, key);
                    if (!path) {
                        return path.takeError();
                    }
                    result->model = path.take();
                    sawModel = true;
                } else if (key == "sampleRate") {
                    auto number = otter::manifest::readPositiveInt(item, key);
                    if (!number) {
                        return number.takeError();
                    }
                    result->sampleRate = number.take();
                } else if (key == "channelCount") {
                    auto number = otter::manifest::readPositiveInt(item, key);
                    if (!number) {
                        return number.takeError();
                    }
                    result->channelCount = number.take();
                } else if (key == "interval") {
                    auto number = otter::manifest::readPositiveDouble(item, key);
                    if (!number) {
                        return number.takeError();
                    }
                    result->interval = number.take();
                } else if (key == "maxSegmentDuration") {
                    auto number = otter::manifest::readPositiveDouble(item, key);
                    if (!number) {
                        return number.takeError();
                    }
                    result->maxSegmentDuration = number.take();
                } else if (key == "voicingThreshold") {
                    auto number = otter::manifest::readUnitDouble(item, key);
                    if (!number) {
                        return number.takeError();
                    }
                    result->defaultVoicingThreshold = number.take();
                } else if (key == "interpolateUnvoiced") {
                    if (!item.isBool()) {
                        return srt::Error(srt::Error::InvalidFormat,
                                          "interpolateUnvoiced must be a boolean");
                    }
                    result->defaultInterpolateUnvoiced = item.toBool();
                } else {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "unknown rmvpe configuration key: " + key);
                }
            }
            if (!sawModel) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the rmvpe configuration needs a model");
            }
            return result;
        }
    };

    class RmvpeProviderPlugin : public otter::AnalysisProviderPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(std::string_view interfaceName, int level, std::string_view variant) override {
            if (interfaceName != F0Api::API_INTERFACE || level != F0Api::API_LEVEL ||
                variant != VARIANT) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this plugin serves only " + std::string(F0Api::API_INTERFACE) +
                                      " level 1 variant " + VARIANT);
            }
            return std::unique_ptr<srt::ContribInterpreter>(new RmvpeProvider());
        }
    };

}

STDC_EXPORT_PLUGIN(RmvpeProviderPlugin)
