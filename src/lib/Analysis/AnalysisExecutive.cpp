#include <otter/Analysis/AnalysisExecutive.h>

#include <string>
#include <utility>

namespace otter {

    AnalysisExtension::AnalysisExtension(AnalysisSpec &spec, std::string id)
        : ContribSpecExtension(spec, std::move(id)) {
    }

    AnalysisExtension::~AnalysisExtension() = default;

    AnalysisExecutive::AnalysisExecutive(AnalysisSpec &spec) : ContribExecutive(spec) {
    }

    AnalysisExecutive::~AnalysisExecutive() = default;

    srt::Expected<void> AnalysisExecutive::quit() {
        return stop();
    }

    srt::Expected<void> AnalysisExecutive::wait() {
        return waitForFinished();
    }

    srt::Expected<void> checkRuntimeOptions(const AnalysisRuntimeOptions &options,
                                            const AnalysisSpec &spec) {
        if (options.interface() != spec.interface() || options.level() != spec.level() ||
            options.variant() != spec.variant()) {
            return srt::Error(
                srt::Error::InvalidArgument,
                "the runtime options were written for " + std::string(options.interface()) +
                    " level " + std::to_string(options.level()) + " variant " +
                    std::string(options.variant()) + ", and this analyzer is " + spec.interface() +
                    " level " + std::to_string(spec.level()) + " variant " + spec.variant());
        }
        return {};
    }

}
