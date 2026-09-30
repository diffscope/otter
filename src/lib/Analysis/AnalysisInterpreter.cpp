#include <otter/Analysis/AnalysisInterpreter.h>

#include <utility>

namespace otter {

    namespace {

        // Import options of any analysis contract, which identify only the contract.
        class EmptyImportOptions : public AnalysisImportOptions {
        public:
            EmptyImportOptions(std::string interfaceName, std::string variant, int level)
                : AnalysisImportOptions(std::move(interfaceName), std::move(variant), level) {
            }
        };

    }

    AnalysisInterpreter::AnalysisInterpreter(std::string interfaceName, int level,
                                             std::string variant)
        : m_interface(std::move(interfaceName)), m_level(level), m_variant(std::move(variant)) {
    }

    const std::string &AnalysisInterpreter::interfaceName() const noexcept {
        return m_interface;
    }

    int AnalysisInterpreter::level() const noexcept {
        return m_level;
    }

    const std::string &AnalysisInterpreter::variant() const noexcept {
        return m_variant;
    }

    srt::Expected<std::unique_ptr<srt::ContribImportOptions>>
        AnalysisInterpreter::createImportOptions(const srt::ContribSpec &target,
                                                 const srt::JsonValue &manifestOptions) const {
        if (!manifestOptions.isNull() &&
            !(manifestOptions.isObject() && manifestOptions.toObject().empty())) {
            return srt::Error(srt::Error::InvalidFormat,
                              "analysis contracts define no import options, and the given "
                              "options would have no effect");
        }
        return std::unique_ptr<srt::ContribImportOptions>(
            new EmptyImportOptions(target.interface(), target.variant(), target.level()));
    }

}
