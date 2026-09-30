#ifndef OTTER_ANALYSISTASK_H
#define OTTER_ANALYSISTASK_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <synthrt/Support/Expected.h>
#include <synthrt/Task/ITask.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/otter_global.h>

namespace otter {

    /// Task interface of one analyzer, built on \c srt::ITask.
    ///
    /// Every analyzer has the same obligations: one execution at a time, a state that a host can
    /// read, a worker thread for the asynchronous entry point, a \c Canceled state for an
    /// execution that reports \c AnalysisError::Cancelled, and a destructor that does not return
    /// while a worker is inside the body. \c srt::ITask already provides the state, the
    /// cancellation flag and the wait. This class adds only the rules that the analysis contracts
    /// impose and the framework's default execution lacks:
    ///
    /// - A stop request applies to the execution that it targets. The claim, which is the
    ///   exclusive right to run one execution, is taken on the caller's thread, in start() and
    ///   startAsync(), and the stop request of the previous execution is discarded at that point.
    ///   A stop() that arrives after startAsync() has returned and before the worker reaches the
    ///   body therefore applies to the new execution instead of being cleared by it. An execution
    ///   started synchronously from inside a callback inherits the claim rather than taking it;
    ///   such an execution discards the stop request after it detects the callback context.
    /// - A callback may start the next execution or destroy the analyzer. The worker that
    ///   delivers the callback releases the execution only after the callback returns, so that a
    ///   waiter also waits for the callback. A start from inside the callback takes over the
    ///   running state from its predecessor rather than being rejected as a second concurrent
    ///   execution. An exception thrown by a callback is caught at the point of delivery, as in
    ///   the framework's default execution, because an exception that leaves a worker thread
    ///   terminates the process.
    /// - The state of a finished execution is determined by its error code, never by the stop
    ///   flag: the code \c AnalysisError::Cancelled yields \c Canceled, any other error yields
    ///   \c Failed, and a result yields \c Succeeded. A caller that branches on the code and a
    ///   caller that reads the state therefore reach the same conclusion, including when a stop
    ///   request arrives just after a body returned a result or failed for another reason. An
    ///   exception thrown by the body is reported as \c AnalysisError::Internal.
    ///
    /// This class implements these rules once, in the library. AnalysisTask adds only the
    /// conversions between the untyped payloads and the payloads of a contract.
    class OTTER_EXPORT AnalysisTaskBase : public srt::ITask {
    public:
        ~AnalysisTaskBase() override;

        /// Returns whether a stop has been requested for the claimed execution.
        ///
        /// A body checks this flag at the granularity at which it can stop, and returns
        /// cancelledError() if the flag is set. A cancelled execution reports \c Canceled and no
        /// result.
        bool cancelled() const noexcept {
            return m_stopRequested.load();
        }

        /// \name Untyped ITask interface
        /// \{

        /// Runs the body on the calling thread.
        ///
        /// A direct call is a synchronous execution and takes the claim itself. A call from the
        /// worker of startAsync() uses the claim that startAsync() already took.
        ///
        /// This function is the public entry point of srt::ITask, so \a input may be a payload of
        /// any contract. Its type() and version() must equal the input type and version for which
        /// the task was created. Any other payload is rejected with \c InvalidArgument before the
        /// claim is taken, so that a rejected call leaves the state of the analyzer unchanged.
        ///
        /// \return The result of the body; an \c InvalidArgument error for a payload of another
        /// contract or during another execution; an \c AnalysisError::Internal error if the body
        /// throws or returns no result; otherwise the error that the body returned.
        srt::Expected<std::unique_ptr<srt::TaskResult>>
            start(const srt::TaskStartInput &input) override;

        /// Runs the body on a worker thread and delivers the outcome to \a callback.
        ///
        /// \return An empty value if the worker started; an \c InvalidArgument error if \a input
        /// or \a callback is empty, if \a input is a payload of another contract, or if another
        /// execution is running; an \c AnalysisError::NoWorker error if no worker thread could be
        /// started.
        srt::Expected<void> startAsync(std::shared_ptr<const srt::TaskStartInput> input,
                                       AsyncCallback callback) override;

        /// Requests cancellation of the current execution.
        ///
        /// \return An empty value. The request cannot fail.
        srt::Expected<void> stop() override;

        /// Waits for the current execution and, for an asynchronous execution, its callback.
        ///
        /// The function returns immediately if it is called from the worker of that execution,
        /// so that a callback that destroys the analyzer does not deadlock.
        ///
        /// \return An empty value.
        srt::Expected<void> waitForFinished() override;
        /// \}

    protected:
        /// Initializes the task for start inputs of payload type \a inputType and version
        /// \a inputVersion, which are those of the contract. \a inputType must outlive this
        /// object, as a string literal does.
        AnalysisTaskBase(const char *inputType, int inputVersion);

        /// Runs the body on an input that has already been checked to belong to the contract.
        ///
        /// The function may throw; an exception is reported as \c AnalysisError::Internal.
        virtual srt::Expected<std::unique_ptr<srt::TaskResult>>
            execute(const srt::TaskStartInput &input) = 0;

        /// Starts a detached thread running \a work.
        ///
        /// Throws \c std::system_error when no thread can be started, which startAsync() reports
        /// as \c AnalysisError::NoWorker. The function is virtual so that tests can simulate
        /// the failure.
        virtual void startWorker(std::function<void()> work);

    private:
        srt::Expected<void> checkInput(const srt::TaskStartInput &input) const;

        bool claimedByThisThread() const;

        /// Returns whether the calling thread is a worker currently delivering its callback.
        ///
        /// The result distinguishes the body of an execution from an execution started inside the
        /// callback of the preceding execution: both run on the same thread and hold the claim.
        bool deliveringCallback() const;

        /// Takes the claim for a synchronous execution on the calling thread.
        srt::Expected<void> claim();

        void release();

        const char *m_inputType;
        int m_inputVersion;

        std::atomic_bool m_stopRequested = false;

        /// Number of claims taken, which lets the worker that delivered a callback determine
        /// whether the running state it is about to release still belongs to its execution. The
        /// counter is shared because the worker may outlive this object.
        std::shared_ptr<std::atomic<std::uint64_t>> m_generation;

        /// Identifier of the worker currently inside its callback, or a default-constructed
        /// identifier if no worker is inside a callback. The value is shared for the same reason
        /// as the generation counter: the callback may destroy this object, and the worker must
        /// still reset the value afterwards.
        std::shared_ptr<std::atomic<std::thread::id>> m_callbackWorker;
    };

    /// Task interface of an analyzer of one contract, which extends AnalysisTaskBase with the
    /// input and result types of the contract.
    ///
    /// \a Input and \a Result are the start input and the result of the contract, and declare
    /// their payload type and version as the static members \c API_INTERFACE and \c API_LEVEL.
    /// The body is a callable that captures the executive; the executive therefore declares this
    /// member last, and its destructor calls stop() and waitForFinished() before any object that
    /// the body reads is destroyed.
    template <class Input, class Result>
    class AnalysisTask : public AnalysisTaskBase {
    public:
        using Body = std::function<srt::Expected<std::unique_ptr<Result>>(const Input &)>;
        using TypedCallback = std::function<void(srt::Expected<std::unique_ptr<Result>> result)>;

        explicit AnalysisTask(Body body)
            : AnalysisTaskBase(Input::API_INTERFACE, Input::API_LEVEL), m_body(std::move(body)) {
        }

        ~AnalysisTask() override = default;

        /// \name Typed interface exposed by the executives
        /// \{

        /// Runs the body synchronously on \a input.
        ///
        /// \return The result of the body, the error of start(), or an
        /// \c AnalysisError::Internal error if the result belongs to another contract.
        srt::Expected<std::unique_ptr<Result>> run(const Input &input) {
            auto result = start(static_cast<const srt::TaskStartInput &>(input));
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
            return startAsync(std::static_pointer_cast<const srt::TaskStartInput>(std::move(input)),
                              [callback = std::move(callback)](
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
        /// Converts an untyped result to the result type of the contract after checking its
        /// payload type and version.
        ///
        /// start() is virtual, so its return value is not guaranteed to come from the body. A
        /// missing result or a result of another contract is an internal fault of the analyzer
        /// and is reported as \c AnalysisError::Internal.
        static srt::Expected<std::unique_ptr<Result>>
            toResult(std::unique_ptr<srt::TaskResult> result) {
            if (!result) {
                return srt::Error(AnalysisError::Internal, "the analysis produced no result");
            }
            if (result->type() != Result::API_INTERFACE || result->version() != Result::API_LEVEL) {
                return srt::Error(AnalysisError::Internal,
                                  "the analysis produced a " + result->type() + " level " +
                                      std::to_string(result->version()) + " result");
            }
            return std::unique_ptr<Result>(static_cast<Result *>(result.release()));
        }

        Body m_body;
    };
}

#endif // OTTER_ANALYSISTASK_H
