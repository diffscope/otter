#ifndef OTTER_ANALYSIS_PROVIDER_ANALYSISTASK_H
#define OTTER_ANALYSIS_PROVIDER_ANALYSISTASK_H

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <synthrt/Support/Expected.h>
#include <synthrt/Task/ITask.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/otter_global.h>

namespace otter {

    /// Task interface of one analyzer, built on \c srt::ITask.
    ///
    /// \c srt::ITask provides the execution claim, callback delivery, cancellation barrier and
    /// state machine. This class adds only the payload identity check and the rule that
    /// \c AnalysisError::Cancelled yields \c Canceled.
    class OTTER_EXPORT AnalysisTaskBase : public srt::ITask {
    public:
        ~AnalysisTaskBase() override;

        /// Returns whether a stop has been requested for the claimed execution.
        ///
        /// A body checks this flag at the granularity at which it can stop, and returns
        /// cancelledError() if the flag is set. A cancelled execution reports \c Canceled and no
        /// result.
        bool cancelled() const noexcept {
            return stopRequested();
        }

    protected:
        /// Initializes the task for start inputs of payload type \a inputType and version
        /// \a inputVersion, which are those of the contract. \a inputType must outlive this
        /// object, as a string literal does.
        AnalysisTaskBase(const char *inputType, int inputVersion);

        /// Runs the body on an input that has already been checked to belong to the contract.
        ///
        /// The function may throw. An exception is reported as \c AnalysisError::Internal.
        virtual srt::Expected<std::unique_ptr<srt::TaskResult>>
            execute(const srt::TaskStartInput &input) = 0;

        srt::Expected<void> validateInput(const srt::TaskStartInput &input) const override;

        State classifyOutcome(
            const srt::Expected<std::unique_ptr<srt::TaskResult>> &outcome) noexcept override;

        srt::Error exceptionError(const std::exception &error) const override;

        srt::Error unknownExceptionError() const override;

        srt::Error workerStartError(const std::system_error &error) const override;

        /// Starts a detached thread running \a work.
        ///
        /// Throws \c std::system_error when no thread can be started, which startAsync() reports
        /// as \c AnalysisError::NoWorker. The function is virtual so that tests can simulate
        /// the failure.
        virtual void startWorker(std::function<void()> work);

    private:
        const char *m_inputType;
        int m_inputVersion;
    };

    /// Task interface of an analyzer of one contract, which extends AnalysisTaskBase with the
    /// input and result types of the contract.
    ///
    /// \a Input and \a Result are the start input and the result of the contract, and declare
    /// their payload identity through the static \c Contract object.
    /// The body is a callable that captures the executive. The executive therefore declares this
    /// member last, and its destructor calls stop() and waitForFinished() before any object that
    /// the body reads is destroyed.
    template <class Input, class Result>
    class AnalysisTask : public AnalysisTaskBase {
    public:
        using Body = std::function<srt::Expected<std::unique_ptr<Result>>(const Input &)>;
        using TypedCallback = std::function<void(srt::Expected<std::unique_ptr<Result>> result)>;

        explicit AnalysisTask(Body body)
            : AnalysisTaskBase(Input::Contract.interfaceId().c_str(), Input::Contract.level()),
              m_body(std::move(body)) {
        }

        ~AnalysisTask() override = default;

        /// \name Typed interface exposed by the executives
        /// \{

        /// Runs the body synchronously on \a input.
        ///
        /// \return The result of the body, the error of start(), or an
        /// \c AnalysisError::ContractViolation error if the result belongs to another contract.
        srt::Expected<std::unique_ptr<Result>> run(const Input &input) {
            auto result = ITask::start(input);
            if (!result) {
                return result.takeError();
            }
            return toResult(result.take());
        }

        /// Runs the body on a worker thread and delivers the typed outcome to \a callback.
        ///
        /// \return An empty value if the worker started, an \c InvalidArgument error if
        /// \a callback is empty, and otherwise the error of startAsync().
        srt::Expected<void> runAsync(std::shared_ptr<const Input> input, TypedCallback callback) {
            if (!callback) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "an asynchronous callback must not be empty");
            }
            return ITask::startAsync(
                std::static_pointer_cast<const srt::TaskStartInput>(std::move(input)),
                [this, callback = std::move(callback)](
                    srt::Expected<std::unique_ptr<srt::TaskResult>> result) mutable {
                    if (!result) {
                        callback(result.takeError());
                        return;
                    }
                    callback(toResult(result.take()));
                });
        }
        /// \}

    protected:
        srt::Expected<std::unique_ptr<srt::TaskResult>>
            execute(const srt::TaskStartInput &input) override {
            auto result = m_body(static_cast<const Input &>(input));
            if (!result) {
                return result.takeError();
            }
            return std::unique_ptr<srt::TaskResult>(result.take().release());
        }

    private:
        // Converts an untyped result to the result type of the contract after checking its
        // payload type and version.
        //
        // start() is virtual, so its return value is not guaranteed to come from the body. A
        // missing result is an internal fault of the analyzer and is reported as
        // \c AnalysisError::Internal, and a result of another contract is reported as
        // \c AnalysisError::ContractViolation.
        srt::Expected<std::unique_ptr<Result>> toResult(std::unique_ptr<srt::TaskResult> result) {
            if (!result) {
                setState(Failed);
                return srt::Error(AnalysisError::Internal, "the analysis produced no result");
            }
            if (result->type() != Result::Contract.interfaceId() ||
                result->version() != Result::Contract.level()) {
                setState(Failed);
                return srt::Error(AnalysisError::ContractViolation,
                                  "the analysis produced a " + result->type() + " level " +
                                      std::to_string(result->version()) + " result");
            }
            return std::unique_ptr<Result>(static_cast<Result *>(result.release()));
        }

        Body m_body;
    };
}

#endif // OTTER_ANALYSIS_PROVIDER_ANALYSISTASK_H
