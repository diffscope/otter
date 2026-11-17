#ifndef OTTER_INFERENCEINTERPRETERS_GAME_GGML_GAMEGGMLINTERPRETER_H
#define OTTER_INFERENCEINTERPRETERS_GAME_GGML_GAMEGGMLINTERPRETER_H

#include <memory>

#include <otter/Analysis/Provider/AnalysisInterpreter.h>

namespace otter::gameGgml {

    inline constexpr char VARIANT[] = "game-ggml";

    /// Spawns the engine on demand and hands out analyzers.
    class GameGgmlInterpreter : public otter::AnalysisInterpreter {
    public:
        GameGgmlInterpreter() = default;
        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override;
    };

}

#endif // OTTER_INFERENCEINTERPRETERS_GAME_GGML_GAMEGGMLINTERPRETER_H
