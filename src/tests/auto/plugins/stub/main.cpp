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
            create(const srt::ContribTriplet &triplet) override {
            if (triplet.variant() != VARIANT) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this plugin serves only the stub variant");
            }
            if (triplet.interfaceId() == F0Api::API_INTERFACE &&
                triplet.level() == F0Api::API_LEVEL) {
                return std::unique_ptr<srt::ContribInterpreter>(new StubF0Interpreter());
            }
            if (triplet.interfaceId() == NoteApi::API_INTERFACE &&
                triplet.level() == NoteApi::API_LEVEL) {
                return std::unique_ptr<srt::ContribInterpreter>(new StubNoteInterpreter());
            }
            if (triplet.interfaceId() == AlignApi::API_INTERFACE &&
                triplet.level() == AlignApi::API_LEVEL) {
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
        const auto declaration = [](const char *interface, int level) {
            return stdc::json::Object{
                {"interface", std::string(interface)},
                {"level",     level                 },
                {"variant",   std::string(VARIANT)  },
            };
        };
        return stdc::json::Object{
            {"name",         "otterstub"                                                 },
            {"interpreters",
             stdc::json::Array{declaration(F0Api::API_INTERFACE, F0Api::API_LEVEL),
                               declaration(NoteApi::API_INTERFACE, NoteApi::API_LEVEL),
                               declaration(AlignApi::API_INTERFACE, AlignApi::API_LEVEL)}},
        };
    }

    STDC_EXPORT_STATIC_PLUGIN(StubPlugin, srt::InferenceInterpreterPlugin::IID, manifest())

}
