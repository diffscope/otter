#include "EnginePipe.h"

#include <cstdio>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <csignal>
#  include <ctime>
#  include <fcntl.h>
#  include <pthread.h>
#endif

namespace otter::ggml {

    namespace {

#if !defined(_WIN32) && !defined(__APPLE__)
        // A write to a pipe whose reader is gone raises SIGPIPE, and its default action ends the
        // process before the write can report EPIPE. stdc::Popen guards the writes of
        // communicate() only, so a provider that writes the stream itself guards the write here.
        // The signal is blocked for this thread rather than ignored for the process, because a
        // disposition is process wide and the host may have one of its own. A SIGPIPE that the
        // write raised is taken off before the mask is restored, and one that was pending before
        // the write is left for the host.
        class BrokenPipeGuard {
        public:
            BrokenPipeGuard() {
                sigemptyset(&m_pipeOnly);
                sigaddset(&m_pipeOnly, SIGPIPE);
                sigset_t pending;
                sigemptyset(&pending);
                m_wasPending = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;
                m_installed = pthread_sigmask(SIG_BLOCK, &m_pipeOnly, &m_previous) == 0;
            }

            ~BrokenPipeGuard() {
                if (!m_installed) {
                    return;
                }
                if (!m_wasPending) {
                    const timespec noWait{};
                    while (sigtimedwait(&m_pipeOnly, nullptr, &noWait) == -1 && errno == EINTR) {
                    }
                }
                pthread_sigmask(SIG_SETMASK, &m_previous, nullptr);
            }

            BrokenPipeGuard(const BrokenPipeGuard &) = delete;
            BrokenPipeGuard &operator=(const BrokenPipeGuard &) = delete;

        private:
            sigset_t m_pipeOnly{};
            sigset_t m_previous{};
            bool m_wasPending = false;
            bool m_installed = false;
        };
#endif

    }

    void prepareEngine(stdc::Popen &process) {
#ifdef _WIN32
        process.creationFlags(CREATE_NO_WINDOW);
#else
        (void) process;
#endif
    }

    bool writeToEngine(stdc::Popen::Stream &input, const void *data, std::size_t size) {
        std::FILE *file = input.file();
        if (!file) {
            return false;
        }
#if defined(__APPLE__)
        // Darwin answers a broken pipe with EPIPE and raises nothing for a descriptor marked so,
        // which leaves the signal mask of the thread alone.
        (void) ::fcntl(fileno(file), F_SETNOSIGPIPE, 1);
#elif !defined(_WIN32)
        const BrokenPipeGuard guard;
#endif
        return std::fwrite(data, 1, size, file) == size && std::fflush(file) == 0;
    }

    bool readLineFromEngine(stdc::Popen::Stream &output, std::string &line) {
        // A refill of the FILE buffer returns what the pipe holds, so the line arrives as soon as
        // its newline does.
        std::FILE *file = output.file();
        line.clear();
        if (!file) {
            return false;
        }
        while (true) {
            const int byte = std::fgetc(file);
            if (byte == EOF) {
                return false;
            }
            if (byte == '\n') {
                return true;
            }
            line.push_back(static_cast<char>(byte));
        }
    }

}
