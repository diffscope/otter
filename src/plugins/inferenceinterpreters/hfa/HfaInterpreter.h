#ifndef OTTER_INFERENCEINTERPRETERS_HFA_HFAINTERPRETER_H
#define OTTER_INFERENCEINTERPRETERS_HFA_HFAINTERPRETER_H

#include <memory>

#include <otter/Analysis/Provider/AnalysisInterpreter.h>
#include <otter/Api/Align/1/AlignApiL1.h>

namespace otter::hfa {

    /// The variant this provider implements.
    inline const srt::ContribContractTriplet Triplet{Api::Align::L1::Contract, "hfa"};

    /// Reads the declaration and the model's own files, and creates analyzers.
    class HfaInterpreter : public otter::AnalysisInterpreter {
    public:
        HfaInterpreter() = default;
        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override;
        /// Reads the configuration block and the model's own files, and checks the declaration
        /// against them.
        ///
        /// Both the declaration and the model's files are available here, so every comparison
        /// between them is performed here, once, when the declaration is loaded. The exports are
        /// read again through the contract's reader instead of being taken from the spec, because
        /// the loader does not guarantee that the exports are interpreted first.
        ///
        /// \return The configuration, or the first error of reading or checking.
        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override;
    };

}

#endif // OTTER_INFERENCEINTERPRETERS_HFA_HFAINTERPRETER_H
