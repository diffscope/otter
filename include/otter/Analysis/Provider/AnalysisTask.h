#ifndef OTTER_ANALYSIS_PROVIDER_ANALYSISTASK_H
#define OTTER_ANALYSIS_PROVIDER_ANALYSISTASK_H

#include <exception>
#include <functional>
#include <memory>
#include <string>
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
            return m_stopToken.stopRequested();
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
            execute(const srt::TaskStartInput &input, const srt::TaskStopToken &token) = 0;

        State classifyOutcome(const srt::Expected<std::unique_ptr<srt::TaskResult>> &outcome,
                              bool stopRequested) noexcept override;

        /// Starts a detached thread running \a work.
        ///
        /// Throws \c std::system_error when no thread can be started, which startAsync() reports
        /// as \c AnalysisError::NoWorker. The function is virtual so that tests can simulate
        /// the failure.
        virtual srt::Expected<void> startWorker(std::function<void()> work);

        void setStopToken(srt::TaskStopToken token) noexcept {
            m_stopToken = std::move(token);
        }

    private:
        srt::TaskStopToken m_stopToken;
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
            auto started = ITask::startAsync(
                std::static_pointer_cast<const srt::TaskStartInput>(std::move(input)),
                [this, callback = std::move(callback)](
                    srt::Expected<std::unique_ptr<srt::TaskResult>> result) mutable {
                    if (!result) {
                        callback(result.takeError());
                        return;
                    }
                    callback(toResult(result.take()));
                });
            if (!started) {
                return started.takeError();
            }
            return {};
        }
        /// \}

    protected:
        srt::Expected<std::unique_ptr<srt::TaskResult>>
            execute(const srt::TaskStartInput &input, const srt::TaskStopToken &token) override {
            setStopToken(token);
            srt::Expected<std::unique_ptr<Result>> result =
                srt::Error(AnalysisError::Internal, "the analysis produced no result");
            try {
                result = m_body(static_cast<const Input &>(input));
            } catch (const std::exception &error) {
                result = srt::Error(AnalysisError::Internal, "the analysis threw an exception: " +
                                                                 std::string(error.what()));
            } catch (...) {
                result = srt::Error(AnalysisError::Internal, "the analysis threw an exception");
            }
            setStopToken({});
            if (!result) {
                return result.takeError();
            }
            if (!*result) {
                return srt::Error(AnalysisError::Internal, "the analysis produced no result");
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
