#ifndef OTTER_INFERENCEINTERPRETERS_TIFA_GGML_TIFAGGMLINTERPRETER_H
#define OTTER_INFERENCEINTERPRETERS_TIFA_GGML_TIFAGGMLINTERPRETER_H

#include <memory>

#include <otter/Analysis/Provider/AnalysisInterpreter.h>
#include <otter/Api/Align/1/AlignApiL1.h>

namespace otter::tifaGgml {

    inline const srt::ContribContractTriplet Triplet{Api::Align::L1::Contract, "tifa-ggml"};

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
