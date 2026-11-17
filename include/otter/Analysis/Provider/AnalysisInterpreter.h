#ifndef OTTER_ANALYSIS_PROVIDER_ANALYSISINTERPRETER_H
#define OTTER_ANALYSIS_PROVIDER_ANALYSISINTERPRETER_H

#include <memory>

#include <synthrt/SVS/InferenceInterpreter.h>

#include <otter/otter_global.h>

namespace otter {

    /// Interprets and executes analysis modules of one contract.
    ///
    /// An analysis module is an inference module. This class is therefore an inference
    /// interpreter. It reads the exports and configuration of a declaration, and
    /// \c createInference creates a runnable analyzer from a loaded declaration. A host calls
    /// \c srt::InferenceSpec::createInference directly, because an analyzer runs without being
    /// imported. Another module may still import an analyzer, in which case the framework creates
    /// the analyzer in the same way on behalf of the importing module.
    class OTTER_EXPORT AnalysisInterpreter : public srt::InferenceInterpreter {
    public:
        ~AnalysisInterpreter() = default;

        /// Creates import options for an import without options, and rejects an import with
        /// non-empty options.
        ///
        /// No Level 1 analysis contract defines import options. Explicitly written options are
        /// rejected because they would have no effect.
        ///
        /// \return Import options that identify the contract of \a target if \a manifestOptions
        /// is null or an empty object, and an \c InvalidFormat error otherwise.
        srt::Expected<std::unique_ptr<srt::ContribImportOptions>>
            createImportOptions(const srt::ContribSpec &target,
                                const srt::JsonValue &manifestOptions) const override;
    };

}

#endif // OTTER_ANALYSIS_PROVIDER_ANALYSISINTERPRETER_H
