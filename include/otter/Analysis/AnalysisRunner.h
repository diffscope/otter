#ifndef OTTER_ANALYSISRUNNER_H
#define OTTER_ANALYSISRUNNER_H

#include <functional>
#include <memory>

#include <synthrt/Support/Expected.h>
#include <synthrt/Task/ITask.h>

#include <otter/otter_global.h>

namespace otter {

    /// The execution lifecycle every analyzer needs.
    ///
    /// One execution at a time, a state a host can read, a worker thread for the asynchronous
    /// entry, and — the part worth having written once — a cancellation that belongs to the
    /// execution it was aimed at.
    ///
    /// That last point is the reason this is a class rather than three copies of a flag. The
    /// obvious placement, clearing the flag where the work begins, is wrong for the asynchronous
    /// entry: \c stop() may legitimately arrive after \c startAsync() has returned but before the
    /// worker reaches the body, and clearing it there would drop that cancellation on the floor.
    /// Claiming the execution happens on the caller's thread, in \c begin(), so that every
    /// cancellation after \c startAsync() returns lands on the execution the caller meant.
    class OTTER_EXPORT AnalysisRunner {
    public:
        AnalysisRunner();

        /// Cancels and waits, so an analyzer destroyed mid-execution does not outlive its worker.
        ~AnalysisRunner();

        /// Returns the state of the current or most recently completed execution.
        srt::ITask::State state() const noexcept;

        /// Returns whether a cancellation has been requested for the claimed execution.
        ///
        /// A body checks this at whatever granularity it can afford to stop at.
        bool cancelled() const noexcept;

        /// Requests cancellation of the claimed execution.
        void cancel() noexcept;

        /// Waits for the asynchronous execution and its callback to finish.
        ///
        /// Returns at once when called from that execution's own callback, so a callback that
        /// starts the next execution does not deadlock on itself.
        void wait() noexcept;

        /// Claims the right to run one execution, discarding a cancellation aimed at the previous.
        ///
        /// Returns false when an execution is already in flight.
        bool begin() noexcept;

        /// Releases the claim and records the outcome.
        ///
        /// A failed execution reports \c Canceled when a cancellation was requested and \c Failed
        /// otherwise: a body that stopped early usually cannot tell the two apart, and the runner
        /// can.
        void end(bool succeeded) noexcept;

        /// Runs \a body on a worker thread.
        ///
        /// \pre \c begin() has returned true and \c end() has not yet been called. The body owns
        ///      calling \c end() before it returns.
        ///
        /// A failure here releases the claim itself, since no body exists to do it.
        srt::Expected<void> spawn(std::function<void()> body);

    private:
        class Impl;
        std::shared_ptr<Impl> _impl;

        STDC_DISABLE_COPY_MOVE(AnalysisRunner)
    };

}

#endif // OTTER_ANALYSISRUNNER_H
