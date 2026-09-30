#include <otter/Analysis/AnalysisTask.h>

#include <exception>
#include <mutex>
#include <system_error>

#include <synthrt/Support/Logging.h>

namespace otter {

    AnalysisTaskBase::AnalysisTaskBase(const char *inputType, int inputVersion)
        : m_inputType(inputType), m_inputVersion(inputVersion),
          m_generation(std::make_shared<std::atomic<std::uint64_t>>(0)),
          m_callbackWorker(std::make_shared<std::atomic<std::thread::id>>()) {
    }

    AnalysisTaskBase::~AnalysisTaskBase() = default;

    srt::Expected<std::unique_ptr<srt::TaskResult>>
        AnalysisTaskBase::start(const srt::TaskStartInput &input) {
        if (auto checked = checkInput(input); !checked) {
            return checked.takeError();
        }
        const bool onWorker = claimedByThisThread();
        if (!onWorker) {
            if (auto claimed = claim(); !claimed) {
                return claimed.takeError();
            }
        } else if (deliveringCallback()) {
            // A synchronous execution started inside the callback of the previous execution. It
            // inherits the claim and is therefore not rejected, but it must not inherit the stop
            // of the previous execution: that request targeted the execution that just ended, and
            // a body reading it here would report an unrequested cancellation. startAsync()
            // clears the flag on the caller's thread for the same reason.
            m_stopRequested.store(false);
            std::lock_guard<std::mutex> lock(m_asyncState->mutex);
            m_asyncState->cancellationRequested = false;
        }
        setState(Running);
        srt::Expected<std::unique_ptr<srt::TaskResult>> result =
            srt::Error(AnalysisError::Internal, "the analysis produced no result");
        try {
            result = execute(input);
        } catch (const std::exception &error) {
            result = srt::Error(AnalysisError::Internal,
                                std::string("the analysis threw an exception: ") + error.what());
        } catch (...) {
            result = srt::Error(AnalysisError::Internal, "the analysis threw an exception");
        }
        if (result && !*result) {
            result = srt::Error(AnalysisError::Internal, "the analysis produced no result");
        }
        // The error code determines the state, so that the code and the state agree regardless
        // of the stop flag: a body that detected the stop reports Cancelled, and a body that
        // finished or failed for another reason before it checked the flag keeps its outcome.
        if (!result) {
            setState(result.error().code() == AnalysisError::Cancelled ? Canceled : Failed);
        } else {
            setState(Succeeded);
        }
        if (!onWorker) {
            release();
        }
        return result;
    }

    srt::Expected<void>
        AnalysisTaskBase::startAsync(std::shared_ptr<const srt::TaskStartInput> input,
                                     AsyncCallback callback) {
        if (!input) {
            return srt::Error(srt::Error::InvalidArgument, "no input was supplied");
        }
        if (!callback) {
            return srt::Error(srt::Error::InvalidArgument,
                              "an asynchronous callback must not be empty");
        }
        if (auto checked = checkInput(*input); !checked) {
            return checked.takeError();
        }
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(m_asyncState->mutex);
            // The worker of the previous execution, while still inside its callback, may start the
            // next execution: it holds the claim and hands the claim over. A start from any other
            // thread while an execution is running is a second concurrent execution and is
            // rejected.
            if (m_asyncState->running && m_asyncState->workerId != std::this_thread::get_id()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this analyzer is already running an execution");
            }
            m_asyncState->running = true;
            m_asyncState->cancellationRequested = false;
            m_asyncState->workerId = {};
            generation = ++*m_generation;
        }
        // Cleared here, on the caller's thread, so that a stop request that arrives immediately
        // after this function returns applies to this execution rather than being cleared by the
        // worker.
        m_stopRequested.store(false);
        setState(Running);

        // The worker holds shared ownership of the state that it accesses after the callback,
        // because the callback may have destroyed the analyzer and this object with it.
        auto asyncState = m_asyncState;
        auto current = m_generation;
        auto callbackWorker = m_callbackWorker;
        try {
            startWorker([this, asyncState, current, callbackWorker, generation,
                         input = std::move(input), callback = std::move(callback)]() mutable {
                {
                    std::lock_guard<std::mutex> lock(asyncState->mutex);
                    asyncState->workerId = std::this_thread::get_id();
                }
                // start() sets the state from the outcome. A stop request that arrives after the
                // body returned does not change the state, because the callback receives that
                // outcome and the state must describe the same outcome.
                auto result = start(*input);
                // Records this worker while the callback runs, so that an execution started inside
                // the callback is distinguished from the body of this execution. The record is
                // shared and cleared through the shared pointer, because the callback may destroy
                // the analyzer before it returns.
                callbackWorker->store(std::this_thread::get_id());
                try {
                    callback(std::move(result));
                } catch (...) {
                    // The framework's default execution guards its callback in the same way. An
                    // exception leaving a worker thread terminates the process. Without this guard
                    // the code below would not run and the claim would never be released; every
                    // later execution would be rejected and every waiter would block indefinitely.
                    srt::logCategory().srtCritical(
                        "an analysis callback threw an exception, which was caught and discarded");
                }
                // Cleared only if the record still identifies this worker. A successor started
                // inside the callback may already be delivering its own callback, and clearing its
                // record would let a start from that callback retain a stop request aimed at
                // another execution.
                auto self = std::this_thread::get_id();
                callbackWorker->compare_exchange_strong(self, std::thread::id());
                // The running state is released after the callback, so that a waiter also waits
                // for the callback. A successor started from inside the callback owns the running
                // state, as the generation counter indicates, and the state is then left unchanged.
                {
                    std::lock_guard<std::mutex> lock(asyncState->mutex);
                    if (current->load() == generation) {
                        asyncState->running = false;
                        asyncState->workerId = {};
                    }
                }
                asyncState->finished.notify_all();
            });
        } catch (const std::system_error &error) {
            // There is no worker to release the claim, so it is released here; otherwise the
            // analyzer would reject every later execution.
            {
                std::lock_guard<std::mutex> lock(m_asyncState->mutex);
                m_asyncState->running = false;
            }
            m_asyncState->finished.notify_all();
            setState(Failed);
            return srt::Error(AnalysisError::NoWorker,
                              std::string("no worker thread could be started: ") + error.what());
        }
        return srt::Expected<void>();
    }

    srt::Expected<void> AnalysisTaskBase::stop() {
        m_stopRequested.store(true);
        requestAsyncCancellation();
        return srt::Expected<void>();
    }

    srt::Expected<void> AnalysisTaskBase::waitForFinished() {
        waitForAsyncExecution();
        return srt::Expected<void>();
    }

    void AnalysisTaskBase::startWorker(std::function<void()> work) {
        std::thread(std::move(work)).detach();
    }

    srt::Expected<void> AnalysisTaskBase::checkInput(const srt::TaskStartInput &input) const {
        if (input.type() != m_inputType || input.version() != m_inputVersion) {
            return srt::Error(srt::Error::InvalidArgument,
                              "this analyzer accepts " + std::string(m_inputType) + " level " +
                                  std::to_string(m_inputVersion) + " input, and the input is " +
                                  input.type() + " level " + std::to_string(input.version()));
        }
        return srt::Expected<void>();
    }

    bool AnalysisTaskBase::claimedByThisThread() const {
        std::lock_guard<std::mutex> lock(m_asyncState->mutex);
        return m_asyncState->running && m_asyncState->workerId == std::this_thread::get_id();
    }

    bool AnalysisTaskBase::deliveringCallback() const {
        return m_callbackWorker->load() == std::this_thread::get_id();
    }

    srt::Expected<void> AnalysisTaskBase::claim() {
        {
            std::lock_guard<std::mutex> lock(m_asyncState->mutex);
            if (m_asyncState->running) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this analyzer is already running an execution");
            }
            m_asyncState->running = true;
            m_asyncState->cancellationRequested = false;
            // The worker identifier names this thread so that a wait from this thread returns
            // immediately instead of deadlocking, and a wait from any other thread waits for the
            // execution.
            m_asyncState->workerId = std::this_thread::get_id();
            ++*m_generation;
        }
        m_stopRequested.store(false);
        return srt::Expected<void>();
    }

    void AnalysisTaskBase::release() {
        {
            std::lock_guard<std::mutex> lock(m_asyncState->mutex);
            m_asyncState->running = false;
            m_asyncState->workerId = {};
        }
        m_asyncState->finished.notify_all();
    }

}
