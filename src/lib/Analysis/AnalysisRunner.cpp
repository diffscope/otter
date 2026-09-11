#include <otter/Analysis/AnalysisRunner.h>

#include <atomic>
#include <system_error>
#include <mutex>
#include <thread>
#include <utility>

namespace otter {

    class AnalysisRunner::Impl {
    public:
        std::mutex mutex;
        std::thread worker;
        std::thread::id workerId;
        std::atomic<srt::ITask::State> state = srt::ITask::Idle;
        std::atomic_bool running = false;
        std::atomic_bool cancelled = false;

        /// Joins the finished worker, unless the caller is that worker.
        void join() noexcept {
            std::thread finished;
            {
                std::lock_guard guard(mutex);
                if (worker.joinable() && workerId == std::this_thread::get_id()) {
                    return;
                }
                finished = std::move(worker);
                worker = std::thread();
            }
            if (finished.joinable()) {
                finished.join();
            }
        }
    };

    AnalysisRunner::AnalysisRunner() : _impl(std::make_shared<Impl>()) {
    }

    AnalysisRunner::~AnalysisRunner() {
        cancel();
        _impl->join();
    }

    srt::ITask::State AnalysisRunner::state() const noexcept {
        return _impl->state.load();
    }

    bool AnalysisRunner::cancelled() const noexcept {
        return _impl->cancelled.load();
    }

    void AnalysisRunner::cancel() noexcept {
        _impl->cancelled.store(true);
    }

    void AnalysisRunner::wait() noexcept {
        _impl->join();
    }

    bool AnalysisRunner::begin() noexcept {
        bool expected = false;
        if (!_impl->running.compare_exchange_strong(expected, true)) {
            return false;
        }
        _impl->cancelled.store(false);
        _impl->state.store(srt::ITask::Running);
        return true;
    }

    void AnalysisRunner::end(bool succeeded) noexcept {
        _impl->state.store(succeeded ? srt::ITask::Succeeded
                                     : (_impl->cancelled.load() ? srt::ITask::Canceled
                                                                : srt::ITask::Failed));
        _impl->running.store(false);
    }

    srt::Expected<void> AnalysisRunner::spawn(std::function<void()> body) {
        // The previous worker has finished its body — begin() said so — but may still be inside a
        // callback, so this waits rather than leaking a thread object.
        _impl->join();

        std::lock_guard guard(_impl->mutex);
        // The worker holds a share of the state so that it stays alive if the analyzer is
        // destroyed while the callback is still running; the destructor waits, but a body that
        // outlives the wait through some other path must still find its flags where it left them.
        auto impl = _impl;
        try {
            _impl->worker = std::thread([impl, body = std::move(body)]() mutable { body(); });
        } catch (const std::system_error &problem) {
            // The body owns calling end(), and there is no body now. Without this the claim taken
            // by begin() is never released and the analyzer refuses every later execution — a
            // thread the system would not give us would otherwise wedge the thing permanently.
            _impl->state.store(srt::ITask::Failed);
            _impl->running.store(false);
            return srt::Error(srt::Error::NotImplemented,
                              std::string("no worker thread could be started: ") +
                                  problem.what());
        }
        _impl->workerId = _impl->worker.get_id();
        return srt::Expected<void>();
    }

}
