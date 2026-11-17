#ifndef OTTER_INFERENCEINTERPRETERS_RMVPE_RMVPEINTERPRETER_H
#define OTTER_INFERENCEINTERPRETERS_RMVPE_RMVPEINTERPRETER_H

#include <memory>

#include <otter/Analysis/Provider/AnalysisInterpreter.h>

namespace otter::rmvpe {

    /// The variant this provider implements.
    inline constexpr char VARIANT[] = "rmvpe";

    /// Reads the declaration and creates analyzers.
    class RmvpeInterpreter : public otter::AnalysisInterpreter {
    public:
        RmvpeInterpreter() = default;
        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override;
    };

}

#endif // OTTER_INFERENCEINTERPRETERS_RMVPE_RMVPEINTERPRETER_H
