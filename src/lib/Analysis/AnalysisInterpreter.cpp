#include <otter/Analysis/Provider/AnalysisInterpreter.h>

#include <otter/Analysis/AnalysisExecutive.h>

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
            new EmptyImportOptions(target.interfaceId(), target.variant(), target.level()));
    }

}
