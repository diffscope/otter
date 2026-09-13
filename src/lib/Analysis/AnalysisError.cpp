#include <otter/Analysis/AnalysisError.h>

#include <string>

namespace otter {

    namespace {

        class AnalysisErrorCategory final : public std::error_category {
        public:
            const char *name() const noexcept override {
                return "otter.analysis";
            }

            std::string message(int condition) const override {
                switch (static_cast<AnalysisError>(condition)) {
                    case AnalysisError::Cancelled:
                        return "the execution was cancelled";
                    case AnalysisError::NoWorker:
                        return "no worker thread could be started";
                }
                return "unknown analysis error";
            }
        };

    }

    const std::error_category &analysisErrorCategory() noexcept {
        static const AnalysisErrorCategory category;
        return category;
    }

}
