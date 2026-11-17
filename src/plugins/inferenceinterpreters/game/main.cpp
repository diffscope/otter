#include <memory>
#include <string_view>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/SVS/InferenceInterpreterPlugin.h>

#include <otter/Api/Note/1/NoteApiL1.h>

#include "GameInterpreter.h"
#include <onnxutil/OnnxSupport.h>

namespace NoteApi = otter::Api::Note::L1;

namespace otter::game {

    class GamePlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(const srt::ContribContractTriplet &triplet) override {
            if (auto served = otter::onnx::checkServed(triplet, Triplet); !served) {
                return served.takeError();
            }
            return std::unique_ptr<srt::ContribInterpreter>(new GameInterpreter());
        }
    };

}

STDC_EXPORT_PLUGIN(otter::game::GamePlugin)
