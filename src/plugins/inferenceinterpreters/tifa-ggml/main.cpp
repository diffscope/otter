#include <memory>
#include <string>
#include <string_view>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/SVS/InferenceInterpreterPlugin.h>

#include <otter/Api/Align/1/AlignApiL1.h>

#include "TifaGgmlInterpreter.h"

namespace AlignApi = otter::Api::Align::L1;

namespace otter::tifaGgml {

    class TifaGgmlPlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(const srt::ContribContractTriplet &triplet) override {
            if (triplet != srt::ContribContractTriplet(AlignApi::API_INTERFACE, AlignApi::API_LEVEL,
                                                       VARIANT)) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this plugin serves the " + std::string(AlignApi::API_INTERFACE) +
                                      " contract at level " + std::to_string(AlignApi::API_LEVEL) +
                                      " only, in the variant " + VARIANT);
            }
            return std::unique_ptr<srt::ContribInterpreter>(new TifaGgmlInterpreter());
        }
    };

}

STDC_EXPORT_PLUGIN(otter::tifaGgml::TifaGgmlPlugin)
