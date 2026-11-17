#include <memory>
#include <string>
#include <string_view>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/SVS/InferenceInterpreterPlugin.h>

#include <otter/Api/Note/1/NoteApiL1.h>

#include "GameGgmlInterpreter.h"

namespace NoteApi = otter::Api::Note::L1;

namespace otter::gameGgml {

    class GameGgmlPlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(const srt::ContribContractTriplet &triplet) override {
            if (triplet != Triplet) {
                return srt::Error(
                    srt::Error::FeatureNotSupported,
                    "this plugin serves the " + std::string(NoteApi::Contract.interfaceId()) +
                        " contract at level " + std::to_string(NoteApi::Contract.level()) +
                        " only, in the variant " + Triplet.variant());
            }
            return std::unique_ptr<srt::ContribInterpreter>(new GameGgmlInterpreter());
        }
    };

}

STDC_EXPORT_PLUGIN(otter::gameGgml::GameGgmlPlugin)
