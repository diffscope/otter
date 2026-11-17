#include <memory>
#include <string>
#include <string_view>

#include <stdcorelib/plugin/plugin.h>
#include <stdcorelib/support/json.h>

#include <synthrt/SVS/InferenceInterpreterPlugin.h>

#include <otter/Api/F0/1/F0ApiL1.h>
#include <otter/Api/Note/1/NoteApiL1.h>
#include <otter/Api/Align/1/AlignApiL1.h>

#include "StubInterpreters.h"

namespace AlignApi = otter::Api::Align::L1;
namespace F0Api = otter::Api::F0::L1;
namespace NoteApi = otter::Api::Note::L1;

namespace otter::stub {

    class StubPlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(const srt::ContribContractTriplet &triplet) override {
            if (triplet.variant() != VARIANT) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this plugin serves only the stub variant");
            }
            if (triplet.interfaceId() == F0Api::Contract.interfaceId() &&
                triplet.level() == F0Api::Contract.level()) {
                return std::unique_ptr<srt::ContribInterpreter>(new StubF0Interpreter());
            }
            if (triplet.interfaceId() == NoteApi::Contract.interfaceId() &&
                triplet.level() == NoteApi::Contract.level()) {
                return std::unique_ptr<srt::ContribInterpreter>(new StubNoteInterpreter());
            }
            if (triplet.interfaceId() == AlignApi::Contract.interfaceId() &&
                triplet.level() == AlignApi::Contract.level()) {
                return std::unique_ptr<srt::ContribInterpreter>(new StubAlignInterpreter());
            }
            return srt::Error(srt::Error::FeatureNotSupported,
                              "this plugin does not serve " + triplet.interfaceId() + " level " +
                                  std::to_string(triplet.level()));
        }
    };

    // The manifest that the plugin.json of a dynamic plugin would carry. A test binary that links
    // these sources finds the stub among the static plugins of the inference category, so a test
    // sets no plugin path and loads no library.
    stdc::json::Value manifest() {
        const auto declaration = [](const std::string &interface, int level) {
            return stdc::json::Object{
                {"interface", std::string(interface)},
                {"level",     level                 },
                {"variant",   std::string(VARIANT)  },
            };
        };
        return stdc::json::Object{
            {"name",         "otterstub"                                                   },
            {"interpreters",
             stdc::json::Array{
                 declaration(F0Api::Contract.interfaceId(), F0Api::Contract.level()),
                 declaration(NoteApi::Contract.interfaceId(), NoteApi::Contract.level()),
                 declaration(AlignApi::Contract.interfaceId(), AlignApi::Contract.level())}},
        };
    }

    STDC_EXPORT_STATIC_PLUGIN(StubPlugin, srt::InferenceInterpreterPlugin::IID, manifest())

}
