#ifndef OTTER_ANALYSISINTERPRETER_H
#define OTTER_ANALYSISINTERPRETER_H

#include <memory>
#include <string>

#include <synthrt/SVS/InferenceInterpreter.h>

#include <otter/Analysis/AnalysisExecutive.h>
#include <otter/otter_global.h>

namespace otter {

    /// Interprets and executes analysis modules of one contract.
    ///
    /// An analysis module is an inference module; this class is therefore an inference
    /// interpreter. It reads the exports and configuration of a declaration, and
    /// \c createInference creates a runnable analyzer from a loaded declaration. A host calls
    /// \c srt::InferenceSpec::createInference directly, because an analyzer runs without being
    /// imported. Another module may still import an analyzer, in which case the framework creates
    /// the analyzer in the same way on behalf of the importing module.
    class OTTER_EXPORT AnalysisInterpreter : public srt::InferenceInterpreter {
    public:
        ~AnalysisInterpreter() = default;

        /// \name Contract served by this interpreter
        /// \{
        /// Returns the interface name of the contract.
        const std::string &interfaceName() const noexcept;

        /// Returns the level of the contract.
        int level() const noexcept;

        /// Returns the variant that implements the contract.
        const std::string &variant() const noexcept;
        /// \}

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

    protected:
        /// Initializes the interpreter of the contract \a interfaceName at \a level, implemented
        /// by \a variant.
        AnalysisInterpreter(std::string interfaceName, int level, std::string variant);

    private:
        std::string m_interface;
        int m_level;
        std::string m_variant;
    };

}

#endif // OTTER_ANALYSISINTERPRETER_H
