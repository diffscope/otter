#include <memory>
#include <string_view>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/SVS/InferenceInterpreterPlugin.h>

#include <otter/Api/F0/1/F0ApiL1.h>

#include "RmvpeInterpreter.h"
#include <onnxutil/OnnxSupport.h>

namespace F0Api = otter::Api::F0::L1;

namespace otter::rmvpe {

    class RmvpePlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(const srt::ContribTriplet &triplet) override {
            if (auto served = otter::onnx::checkServed(
                    triplet, srt::ContribTriplet(F0Api::API_INTERFACE, F0Api::API_LEVEL, VARIANT));
                !served) {
                return served.takeError();
            }
            return std::unique_ptr<srt::ContribInterpreter>(new RmvpeInterpreter());
        }
    };

}

STDC_EXPORT_PLUGIN(otter::rmvpe::RmvpePlugin)
