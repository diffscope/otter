#include <otter/Analysis/AnalysisProvider.h>

#include <utility>

#include <synthrt/Core/ContribImportBinding.h>

namespace otter {

    AnalysisProvider::AnalysisProvider(std::string interfaceName, int level, std::string variant)
        : m_interface(std::move(interfaceName)), m_level(level), m_variant(std::move(variant)) {
    }

    const std::string &AnalysisProvider::interfaceName() const noexcept {
        return m_interface;
    }

    int AnalysisProvider::level() const noexcept {
        return m_level;
    }

    const std::string &AnalysisProvider::variant() const noexcept {
        return m_variant;
    }

    bool AnalysisProvider::serves(const srt::ContribSpec &spec) const {
        return spec.locator().category() == ANALYSIS_CATEGORY && spec.interface() == m_interface &&
               spec.level() == m_level && spec.variant() == m_variant;
    }

    srt::Expected<std::vector<std::unique_ptr<srt::ContribSpecExtension>>>
        AnalysisProvider::createExtensions(srt::ContribSpec &spec) const {
        std::vector<std::unique_ptr<srt::ContribSpecExtension>> result;
        if (!serves(spec)) {
            return result;
        }
        auto extension = createAnalysisExtension(*spec.as<AnalysisSpec>());
        if (!extension) {
            return extension.takeError();
        }
        result.push_back(extension.take());
        return result;
    }

    srt::Expected<std::unique_ptr<srt::ContribImportOptions>>
        AnalysisProvider::createImportOptions(const srt::ContribSpec &target,
                                              const srt::JsonValue &manifestOptions) const {
        // Reached when some other module declares an import whose ref points at an analysis
        // contribution. Level 1 defines no import options for this category, so saying so plainly
        // beats returning an empty options object that would fail one step later without a reason.
        return srt::Error(srt::Error::FeatureNotSupported,
                          "an analysis contribution takes no import options");
    }

    srt::Expected<std::unique_ptr<srt::ContribImportBinding>> AnalysisProvider::createImportBinding(
        srt::ContribSpec &importer, const srt::ContribImport &declaration, srt::ContribSpec &target,
        std::unique_ptr<srt::ContribImportOptions> options) const {
        return srt::Error(srt::Error::FeatureNotSupported,
                          "an analysis contribution cannot be imported; a host creates an "
                          "analyzer from its declaration directly");
    }

}
