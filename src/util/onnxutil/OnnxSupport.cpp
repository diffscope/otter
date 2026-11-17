#include "OnnxSupport.h"

#include <fstream>
#include <iterator>

#include <stdcorelib/path.h>

#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>

#include <dsinfer/Api/Drivers/Onnx/OnnxDriverApi.h>
#include <dsinfer/Inference/InferenceDriver.h>
#include <dsinfer/Inference/InferenceDriverPlugin.h>

#include <analysisutil/ManifestValues.h>

namespace otter::onnx {

    namespace OnnxApi = ds::Api::Onnx;

    srt::Expected<void> checkServed(const srt::ContribContractTriplet &requested,
                                    const srt::ContribContractTriplet &served) {
        if (requested != served) {
            return srt::Error(srt::Error::FeatureNotSupported,
                              "this plugin serves only " + served.toString());
        }
        return srt::Expected<void>();
    }

    srt::Expected<std::unique_ptr<ds::InferenceSession>>
        openSession(srt::InferenceSpec &spec, const std::filesystem::path &path,
                    std::string_view what, bool useCpu) {
        // The ONNX variant and the ONNX driver backend share a name. Therefore that driver can run
        // the variant's models.
        auto service = spec.package().synthUnit().runtimeService(ds::InferenceDriverPlugin::IID,
                                                                 OnnxApi::API_NAME);
        if (service == nullptr) {
            return srt::Error(srt::Error::FeatureNotSupported,
                              "the onnx inference driver is not registered on this unit");
        }
        auto driver = service->as<ds::InferenceDriver>();
        if (driver == nullptr) {
            return srt::Error(srt::Error::FeatureNotSupported,
                              "the service registered as the onnx driver is not an inference "
                              "driver");
        }
        auto session = driver->createSession();
        if (!session) {
            return srt::Error(srt::Error::FeatureNotSupported, "the driver created no session");
        }
        OnnxApi::SessionOpenArgs args;
        args.useCpu = useCpu;
        if (auto opened = session->open(path, args); !opened) {
            return opened.takeError().withContext("cannot open the " + std::string(what) + " " +
                                                  stdc::path::to_utf8(path));
        }
        return session;
    }

    srt::Expected<void>
        readPaths(const srt::JsonObject &object, const std::filesystem::path &directory,
                  std::initializer_list<std::pair<const char *, std::filesystem::path *>> paths,
                  std::initializer_list<const char *> required, std::string_view what) {
        for (const char *key : required) {
            if (object.find(key) == object.end()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  std::string(what) + " requires a " + key);
            }
        }
        for (const auto &[key, member] : paths) {
            const auto it = object.find(key);
            if (it == object.end()) {
                continue;
            }
            auto path = manifest::readPath(it->second, directory, key);
            if (!path) {
                return path.takeError();
            }
            *member = path.take();
        }
        return srt::Expected<void>();
    }

    srt::Expected<std::string> readTextFile(const std::filesystem::path &path,
                                            std::string_view what) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return srt::Error(srt::Error::FileNotOpen,
                              "cannot open " + std::string(what) + " " + stdc::path::to_utf8(path));
        }
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (text.empty()) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " is empty: " + stdc::path::to_utf8(path));
        }
        return text;
    }

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

    srt::Expected<TensorPtr> waveformTensor(const PreparedSamples &samples) {
        auto tensor = ds::Tensor::createFromView<float>(
            {1, static_cast<std::int64_t>(samples.size())},
            stdc::array_view<float>(samples.data(), samples.size()));
        if (!tensor) {
            return tensor.takeError();
        }
        return TensorPtr(tensor.take());
    }

    srt::Expected<std::vector<float>> readFloats(const TensorPtr &tensor, const char *what) {
        if (!tensor) {
            return srt::Error(AnalysisError::ModelFailed, std::string(what) + " is missing");
        }
        if (tensor->dataType() != ds::ITensor::Float) {
            return srt::Error(AnalysisError::ModelMismatch,
                              std::string(what) + " is not a float tensor");
        }
        const auto view = tensor->view<float>();
        return std::vector<float>(view.begin(), view.end());
    }

    srt::Expected<Tensors> run(ds::InferenceSession &session, Tensors inputs,
                               const std::set<std::string> &wanted, std::string_view what,
                               const std::function<bool()> &cancelled) {
        OnnxApi::SessionStartInput request;
        request.inputs = std::move(inputs);
        request.outputs = wanted;

        auto response = session.start(request);
        if (!response) {
            if (cancelled()) {
                return cancelledError();
            }
            return response.takeError().withContext("the " + std::string(what) + " failed");
        }
        // A stop that arrived while the model ran may not have reached the session in time to
        // fail the call. The contract requires a cancelled execution to report no result.
        // Therefore the stop flag is checked again here instead of accepting the completed pass.
        if (cancelled()) {
            return cancelledError();
        }
        auto produced = response.take();
        auto outputs = produced ? produced->as<OnnxApi::SessionResult>() : nullptr;
        if (outputs == nullptr) {
            return srt::Error(AnalysisError::ModelFailed,
                              "the " + std::string(what) + " returned no outputs");
        }
        for (const auto &name : wanted) {
            const auto it = outputs->outputs.find(name);
            if (it == outputs->outputs.end() || !it->second) {
                return srt::Error(AnalysisError::ModelFailed,
                                  "the " + std::string(what) + " did not return " + name);
            }
        }
        return std::move(outputs->outputs);
    }

}
