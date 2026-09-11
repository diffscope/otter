#ifndef OTTER_ANALYSISPROVIDER_H
#define OTTER_ANALYSISPROVIDER_H

#include <memory>
#include <string>
#include <vector>

#include <synthrt/Core/ContribInterpreter.h>

#include <otter/Analysis/AnalysisContrib.h>
#include <otter/Analysis/AnalysisExecutive.h>
#include <otter/otter_global.h>

namespace otter {

    /// Interprets and executes analysis contributions of one contract.
    ///
    /// A provider reads a declaration's exports and configuration, and attaches the
    /// AnalysisExtension that turns it into a runnable analyzer. Imports carry nothing in this
    /// category, which is not the same as being forbidden; both hooks are answered here once so
    /// that every provider treats them alike.
    class OTTER_EXPORT AnalysisProvider : public srt::ContribInterpreter {
    public:
        ~AnalysisProvider() = default;

        /// \name The contract this provider serves
        /// \{
        const std::string &interfaceName() const noexcept;
        int level() const noexcept;
        const std::string &variant() const noexcept;
        /// \}

        /// Returns whether \a spec declares exactly the contract this provider serves.
        bool serves(const srt::ContribSpec &spec) const;

        /// Attaches this provider's extension, and only to contributions it serves.
        ///
        /// The framework offers every registered interpreter every contribution being loaded, so
        /// that a category can supply executives for another category's roles. A provider that
        /// took the offer unconditionally would mount its extension on every declaration in the
        /// package, and a host asking a declaration for the wrong contract would then be handed
        /// one. Sealed here rather than left to each provider, because the mistake is silent.
        srt::Expected<std::vector<std::unique_ptr<srt::ContribSpecExtension>>>
            createExtensions(srt::ContribSpec &spec) const final;

        /// Accepts an import that states no options, and rejects one that states any.
        ///
        /// No Level 1 contract in this category reads import options, and none supplies an
        /// Executive Factory. Neither makes an analysis contribution un-importable: the upper
        /// specification lets a category supply no factory, and the Loader accepts a null one. So
        /// a package that references an analyzer loads, and gets nothing at run time — which is
        /// what it asked for. Options that were actually written are refused, because nothing
        /// would read them.
        srt::Expected<std::unique_ptr<srt::ContribImportOptions>>
            createImportOptions(const srt::ContribSpec &target,
                                const srt::JsonValue &manifestOptions) const override;

        /// Creates a binding with no runtime connection behind it.
        srt::Expected<std::unique_ptr<srt::ContribImportBinding>>
            createImportBinding(srt::ContribSpec &importer, const srt::ContribImport &declaration,
                                srt::ContribSpec &target,
                                std::unique_ptr<srt::ContribImportOptions> options) const override;

    protected:
        AnalysisProvider(std::string interfaceName, int level, std::string variant);

        /// Creates the extension that turns \a spec into a runnable analyzer.
        ///
        /// Called only for a contribution this provider serves.
        virtual srt::Expected<std::unique_ptr<AnalysisExtension>>
            createAnalysisExtension(AnalysisSpec &spec) const = 0;

    private:
        std::string m_interface;
        int m_level;
        std::string m_variant;
    };

}

#endif // OTTER_ANALYSISPROVIDER_H
