#ifndef OTTER_INFERENCEINTERPRETERS_TIFA_GGML_TIFAGGMLINTERPRETER_H
#define OTTER_INFERENCEINTERPRETERS_TIFA_GGML_TIFAGGMLINTERPRETER_H

#include <memory>

#include <otter/Analysis/Provider/AnalysisInterpreter.h>

namespace otter::tifaGgml {

    inline constexpr char VARIANT[] = "tifa-ggml";

    /// Checks the files of a package and hands out analyzers.
    class TifaGgmlInterpreter : public otter::AnalysisInterpreter {
    public:
        TifaGgmlInterpreter() = default;
        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override;
    };

}

#endif // OTTER_INFERENCEINTERPRETERS_TIFA_GGML_TIFAGGMLINTERPRETER_H
