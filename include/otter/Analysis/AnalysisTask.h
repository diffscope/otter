#ifndef OTTER_ANALYSISTASK_H
#define OTTER_ANALYSISTASK_H

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <synthrt/Support/Expected.h>
#include <synthrt/Task/ITask.h>

#include <otter/Analysis/AnalysisError.h>

namespace otter {

    /// The task face of one analyzer, built on \c srt::ITask.
    ///
    /// Every analyzer owes the same things: one execution at a time, a state a host can read, a
    /// worker thread for the asynchronous entry, a \c Canceled state when a stop was seen, and a
    /// destructor that does not return while a worker is inside the body. \c srt::ITask already
    /// carries the state, the cancellation flag and the wait, and this class adds only the two
    /// rules the analysis contracts have that the framework's default execution does not:
    ///
    /// - A stop belongs to the execution it was aimed at. The claim is taken on the caller's
    ///   thread, in start() and startAsync(), and that is where the previous execution's stop is
    ///   discarded. A stop() arriving after startAsync() has returned and before the worker
    ///   reaches the body therefore lands on this execution instead of being cleared by it.
    /// - A callback may start the next execution or destroy the analyzer. The worker that
    ///   delivers the callback lets go of the execution only afterwards, so a waiter sees the
    ///   callback through; and a start from inside the callback takes the running state over
    ///   from its predecessor rather than being refused as a second concurrent execution.
    ///
    /// The body is a callable capturing the executive, so the member is declared last, and the
    /// executive's destructor calls stop() and waitForFinished() before anything the body reads
    /// goes away.
    template <class Input, class Result>
    class AnalysisTask : public srt::ITask {
    public:
        using Body = std::function<srt::Expected<std::unique_ptr<Result>>(const Input &)>;
        using TypedCallback = std::function<void(srt::Expected<std::unique_ptr<Result>> result)>;

        explicit AnalysisTask(Body body)
            : m_body(std::move(body)),
              m_generation(std::make_shared<std::atomic<std::uint64_t>>(0)) {
        }

        ~AnalysisTask() = default;

        /// Whether a stop has been requested for the claimed execution.
        ///
        /// A body checks this at whatever granularity it can afford to stop at, and returns an
        /// error when it is set: a cancelled execution reports \c Canceled and no result.
        bool cancelled() const noexcept {
            return m_stopRequested.load();
        }

        /// \name The untyped ITask face
        /// \{

        /// Runs the body on the calling thread.
        ///
        /// Called directly, this is the synchronous execution and takes the claim itself. Called
        /// by the worker of startAsync(), the claim is already held and passes through.
        srt::Expected<std::unique_ptr<srt::TaskResult>>
            start(const srt::TaskStartInput &input) override {
            const bool onWorker = claimedByThisThread();
            if (!onWorker) {
                if (auto claimed = claim(); !claimed) {
                    return claimed.takeError();
                }
            }
            setState(Running);
            srt::Expected<std::unique_ptr<Result>> result =
                srt::Error(srt::Error::InvalidFormat, "the analysis produced no result");
            try {
                result = m_body(static_cast<const Input &>(input));
            } catch (const std::exception &error) {
                result =
                    srt::Error(srt::Error::InvalidFormat,
                               std::string("the analysis threw an exception: ") + error.what());
            } catch (...) {
                result = srt::Error(srt::Error::InvalidFormat, "the analysis threw an exception");
            }
            // A body that saw the stop returns an error, and the state is what tells that apart
            // from a failure, since synthrt's error codes have no value for it.
            if (!result) {
                setState(m_stopRequested.load() ? Canceled : Failed);
            } else {
                setState(Succeeded);
            }
            if (!onWorker) {
                release();
            }
            if (!result) {
                return result.takeError();
            }
            return std::unique_ptr<srt::TaskResult>(result.take().release());
        }

        srt::Expected<void> startAsync(std::shared_ptr<const srt::TaskStartInput> input,
                                       AsyncCallback callback) override {
            if (!input) {
                return srt::Error(srt::Error::InvalidArgument, "no input was supplied");
            }
            if (!callback) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "an asynchronous callback must not be empty");
            }
            std::uint64_t generation = 0;
            {
                std::lock_guard<std::mutex> lock(m_asyncState->mutex);
                // The worker of the previous execution, still inside its callback, may start the
                // next one: it holds the claim and hands it over. Anyone else while an execution
                // is in flight is a second concurrent execution and is refused.
                if (m_asyncState->running && m_asyncState->workerId != std::this_thread::get_id()) {
                    return srt::Error(srt::Error::InvalidArgument,
                                      "this analyzer is already running an execution");
                }
                m_asyncState->running = true;
                m_asyncState->cancellationRequested = false;
                m_asyncState->workerId = {};
                generation = ++*m_generation;
            }
            // Cleared here, on the caller's thread, so that a stop arriving the instant this
            // returns lands on this execution rather than being cleared by the worker.
            m_stopRequested.store(false);
            setState(Running);

            // The worker holds shares of the state it touches after the callback, since the
            // callback may have destroyed the analyzer, and with it this object.
            auto asyncState = m_asyncState;
            auto current = m_generation;
            try {
                std::thread([this, asyncState, current, generation, input = std::move(input),
                             callback = std::move(callback)]() mutable {
                    {
                        std::lock_guard<std::mutex> lock(asyncState->mutex);
                        asyncState->workerId = std::this_thread::get_id();
                    }
                    auto result = start(*input);
                    {
                        std::lock_guard<std::mutex> lock(asyncState->mutex);
                        if (asyncState->cancellationRequested) {
                            setState(Canceled);
                        }
                    }
                    callback(std::move(result));
                    // Released after the callback, so that a waiter sees the callback through.
                    // A successor started from inside the callback owns the running state now,
                    // which the generation says, and it is left alone.
                    {
                        std::lock_guard<std::mutex> lock(asyncState->mutex);
                        if (current->load() == generation) {
                            asyncState->running = false;
                            asyncState->workerId = {};
                        }
                    }
                    asyncState->finished.notify_all();
                }).detach();
            } catch (const std::system_error &error) {
                // There is no worker to release the claim, so it is released here; otherwise the
                // analyzer would refuse every later execution.
                {
                    std::lock_guard<std::mutex> lock(m_asyncState->mutex);
                    m_asyncState->running = false;
                }
                m_asyncState->finished.notify_all();
                setState(Failed);
                return srt::Error(srt::Error::NotImplemented,
                                  std::string("no worker thread could be started: ") +
                                      error.what());
            }
            return srt::Expected<void>();
        }

        srt::Expected<void> stop() override {
            m_stopRequested.store(true);
            requestAsyncCancellation();
            return srt::Expected<void>();
        }

        /// Waits for the current execution and, for an asynchronous one, its callback.
        ///
        /// Returns at once when called from that execution's own worker, so a callback that
        /// destroys the analyzer does not deadlock on itself.
        srt::Expected<void> waitForFinished() override {
            waitForAsyncExecution();
            return srt::Expected<void>();
        }
        /// \}

        /// \name The typed face the executives expose
        /// \{
        srt::Expected<std::unique_ptr<Result>> run(const Input &input) {
            auto result = start(static_cast<const srt::TaskStartInput &>(input));
            if (!result) {
                return result.takeError();
            }
            return std::unique_ptr<Result>(static_cast<Result *>(result.take().release()));
        }

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
                                  callback(std::unique_ptr<Result>(
                                      static_cast<Result *>(result.take().release())));
                              });
        }
        /// \}

    private:
        bool claimedByThisThread() const {
            std::lock_guard<std::mutex> lock(m_asyncState->mutex);
            return m_asyncState->running && m_asyncState->workerId == std::this_thread::get_id();
        }

        /// Takes the claim for a synchronous execution on the calling thread.
        srt::Expected<void> claim() {
            {
                std::lock_guard<std::mutex> lock(m_asyncState->mutex);
                if (m_asyncState->running) {
                    return srt::Error(srt::Error::InvalidArgument,
                                      "this analyzer is already running an execution");
                }
                m_asyncState->running = true;
                m_asyncState->cancellationRequested = false;
                // Named so that a wait from this thread returns rather than deadlocks, and one
                // from any other thread waits for the run.
                m_asyncState->workerId = std::this_thread::get_id();
                ++*m_generation;
            }
            m_stopRequested.store(false);
            return srt::Expected<void>();
        }

        void release() {
            {
                std::lock_guard<std::mutex> lock(m_asyncState->mutex);
                m_asyncState->running = false;
                m_asyncState->workerId = {};
            }
            m_asyncState->finished.notify_all();
        }

        Body m_body;
        std::atomic_bool m_stopRequested = false;
        /// Counts claims, so the worker that delivered a callback can tell whether the running
        /// state it is about to release is still its own. Shared, since the worker may outlive
        /// this object.
        std::shared_ptr<std::atomic<std::uint64_t>> m_generation;
    };

}

#endif // OTTER_ANALYSISTASK_H
