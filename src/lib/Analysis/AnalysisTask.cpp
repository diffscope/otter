#include <otter/Analysis/Provider/AnalysisTask.h>

#include <utility>

namespace otter {

    AnalysisTaskBase::AnalysisTaskBase(const srt::ContribContract &inputContract)
        : ITask(inputContract) {
    }

    AnalysisTaskBase::~AnalysisTaskBase() = default;

    AnalysisTaskBase::State AnalysisTaskBase::classifyOutcome(
        const srt::Expected<std::unique_ptr<srt::TaskResult>> &outcome, bool) noexcept {
        if (outcome) {
            return Succeeded;
        }
        return outcome.error().code() == AnalysisError::Cancelled ? Canceled : Failed;
    }

    srt::Expected<void> AnalysisTaskBase::startWorker(std::function<void()> work) {
        auto started = ITask::startWorker(std::move(work));
        if (!started) {
            return srt::Error(AnalysisError::NoWorker, started.error().message());
        }
        return {};
    }

}
