#include <memory>
#include <string_view>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/SVS/InferenceInterpreterPlugin.h>

#include <otter/Api/Align/1/AlignApiL1.h>

#include "TifaInterpreter.h"
#include <onnxutil/OnnxSupport.h>

namespace AlignApi = otter::Api::Align::L1;

namespace otter::tifa {

    class TifaPlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(const srt::ContribContractTriplet &triplet) override {
            if (auto served = otter::onnx::checkServed(
                    triplet, srt::ContribContractTriplet(AlignApi::API_INTERFACE,
                                                         AlignApi::API_LEVEL, VARIANT));
                !served) {
                return served.takeError();
            }
            return std::unique_ptr<srt::ContribInterpreter>(new TifaInterpreter());
        }
    };

}

STDC_EXPORT_PLUGIN(otter::tifa::TifaPlugin)
