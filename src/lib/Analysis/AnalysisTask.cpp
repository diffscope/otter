#include <otter/Analysis/Provider/AnalysisTask.h>

#include <thread>
#include <utility>

namespace otter {

    AnalysisTaskBase::AnalysisTaskBase(const char *inputType, int inputVersion)
        : m_inputType(inputType), m_inputVersion(inputVersion) {
    }

    AnalysisTaskBase::~AnalysisTaskBase() = default;

    srt::Expected<void> AnalysisTaskBase::validateInput(const srt::TaskStartInput &input) const {
        if (input.type() != m_inputType || input.version() != m_inputVersion) {
            return srt::Error(srt::Error::InvalidArgument,
                              "this analyzer accepts " + std::string(m_inputType) + " level " +
                                  std::to_string(m_inputVersion) + " input, and the input is " +
                                  input.type() + " level " + std::to_string(input.version()));
        }
        return {};
    }

    AnalysisTaskBase::State AnalysisTaskBase::classifyOutcome(
        const srt::Expected<std::unique_ptr<srt::TaskResult>> &outcome) noexcept {
        if (outcome) {
            return Succeeded;
        }
        return outcome.error().code() == AnalysisError::Cancelled ? Canceled : Failed;
    }

    srt::Error AnalysisTaskBase::exceptionError(const std::exception &error) const {
        return srt::Error(AnalysisError::Internal,
                          "the analysis threw an exception: " + std::string(error.what()));
    }

    srt::Error AnalysisTaskBase::unknownExceptionError() const {
        return srt::Error(AnalysisError::Internal, "the analysis threw an exception");
    }

    srt::Error AnalysisTaskBase::workerStartError(const std::system_error &error) const {
        return srt::Error(AnalysisError::NoWorker,
                          "no worker thread could be started: " + std::string(error.what()));
    }

    void AnalysisTaskBase::startWorker(std::function<void()> work) {
        std::thread(std::move(work)).detach();
    }

}
