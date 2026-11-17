#include "RmvpeInterpreter.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <synthrt/Support/Expected.h>
#include <synthrt/Support/Json.h>

#include <dsinfer/Core/Tensor.h>
#include <dsinfer/Inference/InferenceSession.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/Provider/AnalysisInput.h>
#include <otter/Analysis/Provider/AnalysisInterpreter.h>
#include <otter/Api/F0/1/F0ApiL1.h>
#include <analysisutil/F0Curve.h>
#include <analysisutil/ManifestValues.h>

#include <onnxutil/OnnxSupport.h>

namespace F0Api = otter::Api::F0::L1;

namespace otter::rmvpe {

    namespace {

        /// The sample rate and frame interval of the model's front end. The graph computes its mel
        /// spectrogram at this rate and hop, so a declaration stating other values would misplace
        /// every frame.
        constexpr int MODEL_SAMPLE_RATE = 16000;
        constexpr double MODEL_INTERVAL = 0.01;

        /// The threshold and interpolation setting used if the declaration does not honor the
        /// corresponding knob.
        constexpr double FALLBACK_VOICING_THRESHOLD = 0.03;
        constexpr bool FALLBACK_INTERPOLATE = true;

        /// The tensor names of the exported graph, which form the contract between this variant and
        /// its model: waveform, threshold -> f0, uv.
        namespace tensors {
            constexpr char WAVEFORM[] = "waveform";
            constexpr char THRESHOLD[] = "threshold";
            constexpr char F0[] = "f0";
            constexpr char UV[] = "uv";
        }

        /// The configuration block of this variant, which contains only the model path. The audio
        /// format and the knobs are contract facts and are declared in exports.
        class RmvpeConfiguration : public srt::ContribConfiguration {
        public:
            RmvpeConfiguration()
                : srt::ContribConfiguration({F0Api::API_INTERFACE, F0Api::API_LEVEL, VARIANT}) {
            }

            std::filesystem::path model;
        };

        /// Runs one RMVPE model.
        class RmvpeExecutive : public otter::onnx::OnnxExecutive<F0Api::F0Executive> {
        public:
            RmvpeExecutive(srt::InferenceSpec &spec, std::unique_ptr<ds::InferenceSession> session,
                           const F0Api::F0Schema &schema)
                : OnnxExecutive(spec), m_session(own(std::move(session))), m_schema(schema) {
            }

            ~RmvpeExecutive() override {
                shutDown();
            }

        protected:
            srt::Expected<std::unique_ptr<F0Api::F0Result>>
                run(const F0Api::F0StartInput &input) override {
                const auto &audio = input.audio;
                auto prepared = otter::prepareSamples(
                    audio, m_schema.sampleRate, m_schema.channelCount, m_schema.maxSegmentDuration);
                if (!prepared) {
                    return prepared.takeError();
                }
                const auto waveform = prepared.take();

                // The accepted range is the range that the declaration reports. Therefore a value
                // outside the declared range is rejected instead of being passed to the model.
                auto voicing = otter::chooseKnob(input.voicingThreshold, m_schema.voicingThreshold,
                                                 FALLBACK_VOICING_THRESHOLD, "voicingThreshold");
                if (!voicing) {
                    return voicing.takeError();
                }
                const auto threshold = static_cast<float>(voicing.take());

                if (input.progress && !input.progress(0)) {
                    // The callback answered that the execution does not continue, so the stop is
                    // requested here and the check below reports it.
                    (void) stop();
                }
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }

                otter::onnx::Tensors inputs;
                {
                    auto tensor = otter::onnx::waveformTensor(waveform);
                    if (!tensor) {
                        return tensor.takeError();
                    }
                    inputs[tensors::WAVEFORM] = tensor.take();
                }
                {
                    // The real export declares this input as a true scalar.
                    auto tensor = ds::Tensor::createScalar<float>(threshold, true);
                    if (!tensor) {
                        return tensor.takeError();
                    }
                    inputs[tensors::THRESHOLD] = tensor.take();
                }
                auto ran = runModel(*m_session, std::move(inputs), {tensors::F0, tensors::UV},
                                    "rmvpe model");
                if (!ran) {
                    return ran.takeError();
                }
                const auto outputs = ran.take();

                auto result = std::make_unique<F0Api::F0Result>();
                result->startTime = audio.startTime;
                result->interval = m_schema.interval;

                auto f0 = otter::onnx::readFloats(outputs.at(tensors::F0), tensors::F0);
                if (!f0) {
                    return f0.takeError();
                }
                result->f0 = f0.take();

                const auto &uv = outputs.at(tensors::UV);
                if (uv->dataType() != ds::ITensor::Bool) {
                    return srt::Error(otter::AnalysisError::ModelMismatch, "uv is not boolean");
                }
                const auto uvRaw = uv->rawView();
                if (uvRaw.size() != result->f0.size()) {
                    return srt::Error(otter::AnalysisError::ModelMismatch,
                                      "the rmvpe model returned " + std::to_string(uvRaw.size()) +
                                          " voicing flags for " +
                                          std::to_string(result->f0.size()) + " frames");
                }
                // The model's flag marks the frames that carry no measurement. The flag is
                // therefore inverted to obtain the contract's flag, which marks the frames that
                // carry one.
                result->voiced.resize(uvRaw.size());
                for (std::size_t i = 0; i < uvRaw.size(); ++i) {
                    result->voiced[i] = uvRaw[i] == std::byte{0} ? 1 : 0;
                }

                // The model's own interpolation interprets the mask with the opposite polarity to
                // its documentation. Only one interpretation yields meaningful arithmetic: an
                // unvoiced frame carries no pitch, so the unvoiced frames are the frames to fill
                // in. The mask was inverted above. Therefore voiced here marks the voiced frames.
                if (otter::chooseKnob(input.interpolateUnvoiced, m_schema.interpolateUnvoiced,
                                      FALLBACK_INTERPOLATE)) {
                    otter::support::interpolateUnvoiced(result->f0, result->voiced);
                } else {
                    for (std::size_t i = 0; i < result->f0.size(); ++i) {
                        if (!result->voiced[i]) {
                            result->f0[i] = 0;
                        }
                    }
                }

                if (input.progress && !input.progress(1)) {
                    (void) stop();
                }
                return result;
            }

            ds::InferenceSession *m_session;

            /// Owned by the spec, which outlives every executive created from it.
            const F0Api::F0Schema &m_schema;
        };

    }

    srt::Expected<std::unique_ptr<srt::ContribExports>>
        RmvpeInterpreter::createExports(const srt::ContribSpec &spec) const {
        auto schema = F0Api::readF0Schema(spec, VARIANT);
        if (!schema) {
            return schema.takeError();
        }
        const auto &declared = **schema;
        // The library checks the contract syntax. This variant checks the values that its
        // model supports. The model's front end is built for one rate and one hop, and a
        // declaration stating other values would make the host prepare audio that the graph
        // then misplaces.
        if (declared.sampleRate != MODEL_SAMPLE_RATE) {
            return srt::Error(srt::Error::InvalidFormat, "the rmvpe variant runs at " +
                                                             std::to_string(MODEL_SAMPLE_RATE) +
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
            return srt::Error(srt::Error::InvalidFormat,
                              "the rmvpe variant feeds its model one channel, and the exports "
                              "declare " +
                                  std::to_string(declared.channelCount));
        }
        return std::unique_ptr<srt::ContribExports>(schema.take().release());
    }

    srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
        RmvpeInterpreter::createConfiguration(const srt::ContribSpec &spec) const {
        const auto &value = spec.manifestConfiguration();
        if (!value.isObject()) {
            return srt::Error(srt::Error::InvalidFormat,
                              "the rmvpe configuration must be an object");
        }
        const auto object = value.toObject();
        if (auto checked =
                otter::manifest::rejectUnknownKeys(object, {"model"}, "the rmvpe configuration");
            !checked) {
            return checked.takeError();
        }
        auto result = std::make_unique<RmvpeConfiguration>();
        if (auto read = otter::onnx::readPaths(object, spec.declarationPath().parent_path(),
                                               {
                                                   {"model", &result->model}
        },
                                               {"model"}, "the rmvpe configuration");
            !read) {
            return read.takeError();
        }
        return std::unique_ptr<srt::ContribConfiguration>(result.release());
    }

    srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
        RmvpeInterpreter::createInference(srt::InferenceSpec &spec,
                                          const srt::ContribImportOptions &,
                                          const srt::InferenceRuntimeOptions &) {
        const auto configuration =
            spec.configuration() ? spec.configuration()->as<RmvpeConfiguration>() : nullptr;
        const auto schema = spec.exports() ? spec.exports()->as<F0Api::F0Schema>() : nullptr;
        if (configuration == nullptr || schema == nullptr) {
            return srt::Error(otter::AnalysisError::Internal,
                              "this declaration carries no rmvpe configuration");
        }
        auto session = otter::onnx::openSession(spec, configuration->model, "rmvpe model");
        if (!session) {
            return session.takeError();
        }
        return std::unique_ptr<srt::InferenceExecutive>(
            new RmvpeExecutive(spec, session.take(), *schema));
    }

}
