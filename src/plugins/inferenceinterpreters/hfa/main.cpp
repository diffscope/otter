#include <memory>
#include <string_view>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/SVS/InferenceInterpreterPlugin.h>

#include <otter/Api/Align/1/AlignApiL1.h>

#include "HfaInterpreter.h"
#include <onnxutil/OnnxSupport.h>

namespace AlignApi = otter::Api::Align::L1;

namespace otter::hfa {

    class HfaPlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(const srt::ContribContractTriplet &triplet) override {
            if (auto served = otter::onnx::checkServed(triplet, Triplet); !served) {
                return served.takeError();
            }
            return std::unique_ptr<srt::ContribInterpreter>(new HfaInterpreter());
        }
    };

}

STDC_EXPORT_PLUGIN(otter::hfa::HfaPlugin)
