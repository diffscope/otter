#ifndef OTTER_INFERENCEINTERPRETERS_TIFA_TIFAINTERPRETER_H
#define OTTER_INFERENCEINTERPRETERS_TIFA_TIFAINTERPRETER_H

#include <memory>

#include <otter/Analysis/Provider/AnalysisInterpreter.h>
#include <otter/Api/Align/1/AlignApiL1.h>

namespace otter::tifa {

    /// The variant this provider implements.
    inline const srt::ContribContractTriplet Triplet{Api::Align::L1::Contract, "tifa"};

    /// Opens the five graphs on demand and hands out analyzers.
    class TifaInterpreter : public otter::AnalysisInterpreter {
    public:
        TifaInterpreter() = default;
        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override;
    };

}

#endif // OTTER_INFERENCEINTERPRETERS_TIFA_TIFAINTERPRETER_H
