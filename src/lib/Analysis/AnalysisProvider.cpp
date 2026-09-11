#include <otter/Analysis/AnalysisProvider.h>

#include <utility>

#include <synthrt/Core/ContribImportBinding.h>

namespace otter {

    namespace {

        /// An empty options payload carrying the target's contract.
        ///
        /// Level 1 defines no import options for this category, but "no options" is not the same
        /// as "cannot be imported": the upper specification lets a category supply no Executive
        /// Factory, and the Loader accepts a null one. Refusing here instead would make a package
        /// that legally references an analyzer fail to load.
        class EmptyImportOptions : public srt::ContribImportOptions {
        public:
            EmptyImportOptions(std::string interfaceName, std::string variant, int level)
                : ContribImportOptions(std::move(interfaceName), std::move(variant), level) {
            }
        };

        /// A binding with no runtime connection behind it, for the same reason.
        class InertImportBinding : public srt::ContribImportBinding {
        public:
            InertImportBinding(srt::ContribSpec &importer, const srt::ContribImport &declaration,
                               srt::ContribSpec &target,
                               std::unique_ptr<srt::ContribImportOptions> options)
                : ContribImportBinding(importer, declaration, target, std::move(options)) {
            }

        protected:
            void activate() noexcept override {
            }

            void close() noexcept override {
            }

            srt::Expected<void> wait() override {
                return {};
            }
        };

    }

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
        // Written options are a different matter from absent ones: no contract in this category
        // reads any, so an entry that states them says something nothing will act on.
        if (!manifestOptions.isNull() &&
            !(manifestOptions.isObject() && manifestOptions.toObject().empty())) {
            return srt::Error(srt::Error::InvalidFormat,
                              "an analysis contribution reads no import options, so the options "
                              "given here would have no effect");
        }
        return std::unique_ptr<srt::ContribImportOptions>(
            new EmptyImportOptions(target.interface(), target.variant(), target.level()));
    }

    srt::Expected<std::unique_ptr<srt::ContribImportBinding>> AnalysisProvider::createImportBinding(
        srt::ContribSpec &importer, const srt::ContribImport &declaration, srt::ContribSpec &target,
        std::unique_ptr<srt::ContribImportOptions> options) const {
        return std::unique_ptr<srt::ContribImportBinding>(
            new InertImportBinding(importer, declaration, target, std::move(options)));
    }

}
