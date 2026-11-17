#ifndef OTTER_INFERENCEINTERPRETERS_GAME_GAMEINTERPRETER_H
#define OTTER_INFERENCEINTERPRETERS_GAME_GAMEINTERPRETER_H

#include <memory>

#include <otter/Analysis/Provider/AnalysisInterpreter.h>

namespace otter::game {

    inline constexpr char VARIANT[] = "game";

    class GameInterpreter : public otter::AnalysisInterpreter {
    public:
        GameInterpreter() = default;
        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override;
    };

}

#endif // OTTER_INFERENCEINTERPRETERS_GAME_GAMEINTERPRETER_H
