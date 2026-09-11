#include <otter/Analysis/AnalysisExecutive.h>

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

}
