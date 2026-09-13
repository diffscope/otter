#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/AnalysisInput.h>
#include <otter/Analysis/AnalysisProvider.h>
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

    /// What the model's own front end is built for. The graph computes its mel spectrogram at
    /// this rate and hop, so a declaration stating anything else would place every frame wrongly.
    constexpr int MODEL_SAMPLE_RATE = 16000;
    constexpr double MODEL_INTERVAL = 0.01;

    /// The threshold and interpolation used when the declaration honors no such knob.
    constexpr double FALLBACK_VOICING_THRESHOLD = 0.03;
    constexpr bool FALLBACK_INTERPOLATE = true;

    /// What this variant reads from its configuration block: the model, and nothing else. The
    /// audio format and the knobs are contract facts and live in exports.
    class RmvpeConfiguration : public srt::ContribConfiguration {
    public:
        RmvpeConfiguration()
            : srt::ContribConfiguration(F0Api::API_INTERFACE, VARIANT, F0Api::API_LEVEL) {
        }

        std::filesystem::path model;
    };

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
            f0[i] = static_cast<float>(
                f0[prev] * std::exp(ratio * static_cast<double>(i - prev) / (next - prev)));
        }
    }

    /// Runs one RMVPE model.
    class RmvpeExecutive : public F0Api::F0Executive {
    public:
        RmvpeExecutive(otter::AnalysisSpec &spec, std::unique_ptr<ds::InferenceSession> session,
                       const F0Api::F0Schema &schema)
            : F0Executive(spec), m_session(std::move(session)), m_sampleRate(schema.sampleRate),
              m_channelCount(schema.channelCount), m_interval(schema.interval),
              m_maxSegmentDuration(schema.maxSegmentDuration),
              m_voicingThreshold(schema.voicingThreshold),
              m_interpolate(schema.interpolateUnvoiced) {
        }

        ~RmvpeExecutive() {
            // The contract's destructor waits too, but it runs after this body, and the session
            // must not be torn down under a forward pass still in flight.
            (void) stop();
            (void) waitForFinished();
            if (m_session) {
                m_session->stop();
                m_session->close();
            }
        }

        srt::Expected<void> stop() override {
            (void) F0Executive::stop();
            // The session is what is actually blocking, so it hears about it too. A model already
            // inside a forward pass is not interruptible everywhere, which is why the task's
            // flag is checked around the call as well. A session with nothing running answers
            // with an error that means only that, so it is not passed on as a failed stop.
            if (m_session) {
                (void) m_session->stop();
            }
            return srt::Expected<void>();
        }

        srt::Expected<void> waitForFinished() override {
            (void) F0Executive::waitForFinished();
            if (m_session) {
                return m_session->waitForFinished();
            }
            return srt::Expected<void>();
        }

    protected:
        srt::Expected<std::unique_ptr<F0Api::F0Result>>
            run(const F0Api::F0StartInput &input) override {
            const auto &audio = input.audio;
            auto prepared =
                otter::prepareSamples(audio, m_sampleRate, m_channelCount, m_maxSegmentDuration);
            if (!prepared) {
                return prepared.takeError();
            }
            const auto waveform = prepared.take();

            // The range is the one the declaration reports, so a value the module said it would
            // not take is refused rather than passed to the model to do something with.
            auto voicing = otter::chooseKnob(input.voicingThreshold, m_voicingThreshold,
                                             FALLBACK_VOICING_THRESHOLD, "voicingThreshold");
            if (!voicing) {
                return voicing.takeError();
            }
            const auto threshold = static_cast<float>(voicing.take());

            if (input.progress) {
                input.progress(0);
            }
            if (cancelled()) {
                return srt::Error(otter::AnalysisError::Cancelled, "the execution was cancelled");
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
                // A true scalar, which is what the real export declares for this input.
                auto tensor = ds::Tensor::createScalar<float>(threshold, true);
                if (!tensor) {
                    return tensor.takeError();
                }
                request.inputs["threshold"] = tensor.take();
            }
            request.outputs = {"f0", "uv"};

            auto response = m_session->start(request);
            if (!response) {
                if (cancelled()) {
                    return srt::Error(otter::AnalysisError::Cancelled,
                                      "the execution was cancelled");
                }
                return response.takeError().withContext("the rmvpe model failed");
            }
            // A stop that arrived while the model ran may not have reached it in time to fail the
            // call. The contract says a cancelled execution reports Canceled and no result, so it
            // is asked again here rather than letting a completed pass count as success.
            if (cancelled()) {
                return srt::Error(otter::AnalysisError::Cancelled, "the execution was cancelled");
            }
            auto taskResult = response.take();
            auto outputs = taskResult ? taskResult->as<OnnxApi::SessionResult>() : nullptr;
            if (outputs == nullptr) {
                return srt::Error(srt::Error::InvalidFormat, "the rmvpe model returned no outputs");
            }

            auto result = std::make_unique<F0Api::F0Result>();
            result->startTime = audio.startTime;
            result->interval = m_interval;

            const auto f0It = outputs->outputs.find("f0");
            if (f0It == outputs->outputs.end() || !f0It->second) {
                return srt::Error(srt::Error::InvalidFormat, "the rmvpe model did not return f0");
            }
            if (f0It->second->dataType() != ds::ITensor::Float) {
                return srt::Error(srt::Error::InvalidFormat, "f0 is not floating point");
            }
            const auto f0View = f0It->second->view<float>();
            result->f0.assign(f0View.begin(), f0View.end());

            const auto uvIt = outputs->outputs.find("uv");
            if (uvIt == outputs->outputs.end() || !uvIt->second) {
                return srt::Error(srt::Error::InvalidFormat, "the rmvpe model did not return uv");
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

            if (otter::chooseKnob(input.interpolateUnvoiced, m_interpolate, FALLBACK_INTERPOLATE)) {
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
        CommonApi::Knob m_voicingThreshold;
        CommonApi::FlagKnob m_interpolate;
    };

    /// Opens the model on demand and hands out analyzers.
    class RmvpeExtension : public otter::AnalysisExtension {
    public:
        explicit RmvpeExtension(otter::AnalysisSpec &spec)
            : AnalysisExtension(
                  spec,
                  srt::ContribSpecExtensionTraits<otter::AnalysisSpec, F0Api::F0Executive>::ID) {
        }

        srt::Expected<std::unique_ptr<otter::AnalysisExecutive>>
            createAnalyzer(const otter::AnalysisRuntimeOptions &runtimeOptions) override {
            if (auto checked = otter::checkRuntimeOptions(runtimeOptions, spec()); !checked) {
                return checked.takeError();
            }
            const auto configuration =
                spec().configuration() ? spec().configuration()->as<RmvpeConfiguration>() : nullptr;
            const auto schema =
                spec().exports() ? spec().exports()->as<F0Api::F0Schema>() : nullptr;
            if (configuration == nullptr || schema == nullptr) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "this declaration carries no rmvpe configuration");
            }

            auto service = spec().package().synthUnit().runtimeService(
                ds::InferenceDriverPlugin::IID, BACKEND);
            if (service == nullptr) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "the onnx inference driver is not registered on this unit");
            }
            auto driver = service->as<ds::InferenceDriver>();
            if (driver == nullptr) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "the service registered as the onnx driver is not one");
            }

            auto session = driver->createSession();
            if (!session) {
                return srt::Error(srt::Error::FeatureNotSupported, "the driver created no session");
            }
            OnnxApi::SessionOpenArgs args;
            if (auto opened = session->open(configuration->model, args); !opened) {
                return opened.takeError().withContext("cannot open the rmvpe model " +
                                                      stdc::path::to_utf8(configuration->model));
            }
            return std::unique_ptr<otter::AnalysisExecutive>(
                new RmvpeExecutive(spec(), std::move(session), *schema));
        }
    };

    class RmvpeProvider : public otter::AnalysisProvider {
    public:
        RmvpeProvider() : AnalysisProvider(F0Api::API_INTERFACE, F0Api::API_LEVEL, VARIANT) {
        }

        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override {
            auto schema = F0Api::readF0Schema(spec, VARIANT);
            if (!schema) {
                return schema.takeError();
            }
            const auto &declared = **schema;
            // The contract syntax is the library's; what this model can honor is this variant's
            // to check. Its front end is built for one rate and one hop, and a declaration that
            // says otherwise would make the host prepare audio the graph then misplaces.
            if (declared.sampleRate != MODEL_SAMPLE_RATE) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the rmvpe variant runs at " + std::to_string(MODEL_SAMPLE_RATE) +
                                      " Hz; the exports declare " +
                                      std::to_string(declared.sampleRate));
            }
            if (declared.interval != MODEL_INTERVAL) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the rmvpe variant produces a frame every " +
                                      std::to_string(MODEL_INTERVAL) + " s; the exports declare " +
                                      std::to_string(declared.interval));
            }
            if (declared.channelCount != 1) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "the rmvpe variant feeds its model one channel, and the exports "
                                  "declare " +
                                      std::to_string(declared.channelCount));
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

    protected:
        srt::Expected<std::unique_ptr<otter::AnalysisExtension>>
            createAnalysisExtension(otter::AnalysisSpec &spec) const override {
            return std::unique_ptr<otter::AnalysisExtension>(new RmvpeExtension(spec));
        }

    private:
        static srt::Expected<std::unique_ptr<RmvpeConfiguration>>
            readConfiguration(const srt::ContribSpec &spec) {
            const auto &value = spec.manifestConfiguration();
            if (!value.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the rmvpe configuration must be an object");
            }
            const auto object = value.toObject();
            if (auto checked = otter::manifest::rejectUnknownKeys(object, {"model"},
                                                                  "the rmvpe configuration");
                !checked) {
                return checked.takeError();
            }
            const auto model = object.find("model");
            if (model == object.end()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the rmvpe configuration needs a model");
            }
            auto path = otter::manifest::readPath(model->second,
                                                  spec.declarationPath().parent_path(), "model");
            if (!path) {
                return path.takeError();
            }
            auto result = std::make_unique<RmvpeConfiguration>();
            result->model = path.take();
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
