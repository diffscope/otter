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
            if (triplet !=
                srt::ContribContractTriplet(NoteApi::API_INTERFACE, NoteApi::API_LEVEL, VARIANT)) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this plugin serves the " + std::string(NoteApi::API_INTERFACE) +
                                      " contract at level " + std::to_string(NoteApi::API_LEVEL) +
                                      " only, in the variant " + VARIANT);
            }
            return std::unique_ptr<srt::ContribInterpreter>(new GameGgmlInterpreter());
        }
    };

}

STDC_EXPORT_PLUGIN(otter::gameGgml::GameGgmlPlugin)
