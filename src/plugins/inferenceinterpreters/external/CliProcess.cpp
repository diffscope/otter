#include "CliProcess.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <system_error>

#include <synthrt/Support/Error.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace otter::cli {

    namespace {

        std::string quotedArgument(const std::string &argument) {
            // The Windows rule for one argument of a command line: the argument is wrapped in
            // quotes, quotes inside it are doubled, and every backslash run that a quote follows
            // is doubled with the quote. These providers pass paths and numbers, so the interesting
            // case is the plain quoted path of a Windows drive.
            std::string result;
            result.reserve(argument.size() + 2);
            result.push_back('"');
            std::size_t backslashes = 0;
            for (const char c : argument) {
                if (c == '\\') {
                    ++backslashes;
                    result.push_back(c);
                    continue;
                }
                if (c == '"') {
                    result.append(backslashes + 1, '\\');
                    backslashes = 0;
                } else {
                    backslashes = 0;
                }
                result.push_back(c);
            }
            result.append(backslashes, '\\');
            result.push_back('"');
            return result;
        }

        std::string commandLineOf(const std::vector<std::string> &commandLine) {
            std::string result;
            for (std::size_t i = 0; i < commandLine.size(); ++i) {
                if (i) {
                    result.push_back(' ');
                }
                result += quotedArgument(commandLine[i]);
            }
            return result;
        }

        srt::Error lastSystemError(const std::string &what) {
#ifdef _WIN32
            return srt::Error(
                std::error_code(static_cast<int>(GetLastError()), std::system_category()), what);
#else
            return srt::Error(std::error_code(errno, std::generic_category()), what);
#endif
        }

    }

#ifdef _WIN32

    namespace {

        struct Handles {
            // Serializes the process handle against requestCancel(), which another thread may call
            // while the owning thread blocks in write(), readLine() or wait(). Blocking waits are
            // taken outside the lock so a cancellation is never held off by them.
            std::mutex mutex;
            HANDLE process = nullptr;
            HANDLE stdinWrite = nullptr;
            HANDLE stdoutRead = nullptr;
        };

    }

    Process::Process() : m_impl(new Handles{}) {
    }

    Process::~Process() {
        terminate();
        close();
        delete static_cast<Handles *>(m_impl);
    }

    Process::Process(Process &&other) noexcept : m_impl(other.m_impl) {
        other.m_impl = new Handles{};
    }

    Process &Process::operator=(Process &&other) noexcept {
        if (this != &other) {
            terminate();
            close();
            delete static_cast<Handles *>(m_impl);
            m_impl = other.m_impl;
            other.m_impl = new Handles{};
        }
        return *this;
    }

    void Process::close() {
        auto *handles = static_cast<Handles *>(m_impl);
        std::lock_guard<std::mutex> lock(handles->mutex);
        if (handles->stdinWrite != nullptr) {
            CloseHandle(handles->stdinWrite);
            handles->stdinWrite = nullptr;
        }
        if (handles->stdoutRead != nullptr) {
            CloseHandle(handles->stdoutRead);
            handles->stdoutRead = nullptr;
        }
        if (handles->process != nullptr) {
            CloseHandle(handles->process);
            handles->process = nullptr;
        }
    }

    srt::Expected<void> Process::start(const std::vector<std::string> &commandLine) {
        auto *handles = static_cast<Handles *>(m_impl);
        std::lock_guard<std::mutex> lock(handles->mutex);
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE stdinRead = nullptr;
        HANDLE stdinWrite = nullptr;
        HANDLE stdoutRead = nullptr;
        HANDLE stdoutWrite = nullptr;
        if (!CreatePipe(&stdinRead, &stdinWrite, &inheritable, 0) ||
            !CreatePipe(&stdoutRead, &stdoutWrite, &inheritable, 0)) {
            return lastSystemError("cannot create the pipes of the engine process");
        }
        // The ends this object keeps must not be inherited, or the pipes would never reach EOF.
        SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdinRead;
        startup.hStdOutput = stdoutWrite;
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::string line = commandLineOf(commandLine);
        PROCESS_INFORMATION information{};
        if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &information)) {
            CloseHandle(stdinRead);
            CloseHandle(stdoutWrite);
            CloseHandle(stdinWrite);
            CloseHandle(stdoutRead);
            return lastSystemError("cannot spawn the engine process: " + line);
        }
        CloseHandle(stdinRead);
        CloseHandle(stdoutWrite);
        CloseHandle(information.hThread);
        handles->process = information.hProcess;
        handles->stdinWrite = stdinWrite;
        handles->stdoutRead = stdoutRead;
        return srt::Expected<void>();
    }

    srt::Expected<void> Process::write(const void *data, std::size_t size) {
        auto *handles = static_cast<Handles *>(m_impl);
        auto *bytes = static_cast<const char *>(data);
        while (size > 0) {
            DWORD written = 0;
            if (!WriteFile(handles->stdinWrite, bytes, static_cast<DWORD>(size), &written,
                           nullptr) ||
                written == 0) {
                return lastSystemError("cannot write to the engine process");
            }
            bytes += written;
            size -= written;
        }
        return srt::Expected<void>();
    }

    srt::Expected<std::string> Process::readLine() {
        auto *handles = static_cast<Handles *>(m_impl);
        std::string line;
        char byte = 0;
        DWORD read = 0;
        while (true) {
            if (!ReadFile(handles->stdoutRead, &byte, 1, &read, nullptr) || read == 0) {
                return lastSystemError("the engine process closed its output");
            }
            if (byte == '\n') {
                break;
            }
            line.push_back(byte);
        }
        return line;
    }

    srt::Expected<int> Process::wait() {
        auto *handles = static_cast<Handles *>(m_impl);
        HANDLE process = nullptr;
        {
            std::lock_guard<std::mutex> lock(handles->mutex);
            process = handles->process;
        }
        if (process == nullptr) {
            return srt::Error(srt::Error::InvalidArgument, "the engine process is not running");
        }
        // The wait is taken outside the lock, so a cancellation request from another thread is
        // never held off by a child that is still running.
        WaitForSingleObject(process, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process, &code);
        {
            std::lock_guard<std::mutex> lock(handles->mutex);
            if (handles->process == process) {
                handles->process = nullptr;
            }
            CloseHandle(process);
        }
        return static_cast<int>(code);
    }

    srt::Expected<void> Process::requestCancel() {
        auto *handles = static_cast<Handles *>(m_impl);
        std::lock_guard<std::mutex> lock(handles->mutex);
        if (handles->process != nullptr) {
            TerminateProcess(handles->process, static_cast<UINT>(-1));
        }
        return srt::Expected<void>();
    }

    srt::Expected<void> Process::terminate() {
        auto *handles = static_cast<Handles *>(m_impl);
        std::lock_guard<std::mutex> lock(handles->mutex);
        if (handles->process != nullptr) {
            TerminateProcess(handles->process, static_cast<UINT>(-1));
            WaitForSingleObject(handles->process, 5000);
            CloseHandle(handles->process);
            handles->process = nullptr;
        }
        if (handles->stdinWrite != nullptr) {
            CloseHandle(handles->stdinWrite);
            handles->stdinWrite = nullptr;
        }
        if (handles->stdoutRead != nullptr) {
            // Released here as well, because an object reused after a cancelled execution spawns
            // again and would otherwise overwrite the old read end without closing it.
            CloseHandle(handles->stdoutRead);
            handles->stdoutRead = nullptr;
        }
        return srt::Expected<void>();
    }

    bool Process::alive() const noexcept {
        auto *handles = static_cast<const Handles *>(m_impl);
        return handles->process != nullptr;
    }

#else // !_WIN32

    namespace {

        struct Handles {
            // Serializes the child's identity against requestCancel(), which another thread may
            // call while the owning thread blocks in write(), readLine() or wait(). Blocking waits
            // are taken outside the lock so a cancellation is never held off by them.
            std::mutex mutex;
            pid_t pid = -1;
            int stdinWrite = -1;
            int stdoutRead = -1;
        };

        srt::Expected<void> writeAll(int fd, const void *data, std::size_t size) {
            // A write to a pipe whose reader is gone raises SIGPIPE, whose default action kills
            // the host before the write can report EPIPE. The signal is blocked for this thread
            // and a request that arrived during the write is consumed before the mask is
            // restored, so the host never dies of a cancellation it asked for.
            sigset_t blocked;
            sigset_t previous;
            sigemptyset(&blocked);
            sigaddset(&blocked, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &blocked, &previous);

            const auto finish = [&blocked, &previous] {
                const timespec zero{0, 0};
                while (sigtimedwait(&blocked, nullptr, &zero) == SIGPIPE) {
                }
                pthread_sigmask(SIG_SETMASK, &previous, nullptr);
            };

            auto *bytes = static_cast<const char *>(data);
            while (size > 0) {
                const ssize_t written = ::write(fd, bytes, size);
                if (written < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    const auto failure = lastSystemError("cannot write to the engine process");
                    finish();
                    return failure;
                }
                bytes += written;
                size -= static_cast<std::size_t>(written);
            }
            finish();
            return srt::Expected<void>();
        }

    }

    Process::Process() : m_impl(new Handles{}) {
    }

    Process::~Process() {
        terminate();
        close();
        delete static_cast<Handles *>(m_impl);
    }

    Process::Process(Process &&other) noexcept : m_impl(other.m_impl) {
        other.m_impl = new Handles{};
    }

    Process &Process::operator=(Process &&other) noexcept {
        if (this != &other) {
            terminate();
            close();
            delete static_cast<Handles *>(m_impl);
            m_impl = other.m_impl;
            other.m_impl = new Handles{};
        }
        return *this;
    }

    void Process::close() {
        auto *handles = static_cast<Handles *>(m_impl);
        std::lock_guard<std::mutex> lock(handles->mutex);
        if (handles->stdinWrite >= 0) {
            ::close(handles->stdinWrite);
            handles->stdinWrite = -1;
        }
        if (handles->stdoutRead >= 0) {
            ::close(handles->stdoutRead);
            handles->stdoutRead = -1;
        }
        handles->pid = -1;
    }

    srt::Expected<void> Process::start(const std::vector<std::string> &commandLine) {
        auto *handles = static_cast<Handles *>(m_impl);
        std::lock_guard<std::mutex> lock(handles->mutex);
        // The argument vector is built before the fork: after it, a multithreaded host could find
        // an allocator lock held by another thread and deadlock in the child before execvp.
        std::vector<char *> argv;
        argv.reserve(commandLine.size() + 1);
        for (const auto &argument : commandLine) {
            argv.push_back(const_cast<char *>(argument.c_str()));
        }
        argv.push_back(nullptr);

        int stdinPipe[2] = {-1, -1};
        int stdoutPipe[2] = {-1, -1};
        const auto closePipes = [&stdinPipe, &stdoutPipe] {
            for (const int fd : {stdinPipe[0], stdinPipe[1], stdoutPipe[0], stdoutPipe[1]}) {
                if (fd >= 0) {
                    ::close(fd);
                }
            }
        };
        if (pipe(stdinPipe) != 0) {
            return lastSystemError("cannot create the pipes of the engine process");
        }
        if (pipe(stdoutPipe) != 0) {
            const auto failure = lastSystemError("cannot create the pipes of the engine process");
            closePipes();
            return failure;
        }
        // The ends this object keeps must not survive an exec of another analyzer's child, or that
        // child would hold this pipe open and a read here would never see its own child's exit.
        // dup2 clears the flag on the standard streams it duplicates.
        for (const int fd : {stdinPipe[0], stdinPipe[1], stdoutPipe[0], stdoutPipe[1]}) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        const pid_t pid = fork();
        if (pid < 0) {
            const auto failure = lastSystemError("cannot spawn the engine process");
            closePipes();
            return failure;
        }
        if (pid == 0) {
            dup2(stdinPipe[0], STDIN_FILENO);
            dup2(stdoutPipe[1], STDOUT_FILENO);
            closePipes();
            execvp(argv.front(), argv.data());
            _exit(127);
        }
        ::close(stdinPipe[0]);
        stdinPipe[0] = -1;
        ::close(stdoutPipe[1]);
        stdoutPipe[1] = -1;
        handles->pid = pid;
        handles->stdinWrite = stdinPipe[1];
        handles->stdoutRead = stdoutPipe[0];
        return srt::Expected<void>();
    }

    srt::Expected<void> Process::write(const void *data, std::size_t size) {
        auto *handles = static_cast<Handles *>(m_impl);
        return writeAll(handles->stdinWrite, data, size);
    }

    srt::Expected<std::string> Process::readLine() {
        auto *handles = static_cast<Handles *>(m_impl);
        std::string line;
        char byte = 0;
        while (true) {
            const ssize_t read = ::read(handles->stdoutRead, &byte, 1);
            if (read < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return lastSystemError("the engine process closed its output");
            }
            if (read == 0) {
                return lastSystemError("the engine process closed its output");
            }
            if (byte == '\n') {
                break;
            }
            line.push_back(byte);
        }
        return line;
    }

    srt::Expected<int> Process::wait() {
        auto *handles = static_cast<Handles *>(m_impl);
        pid_t pid = -1;
        {
            std::lock_guard<std::mutex> lock(handles->mutex);
            pid = handles->pid;
        }
        if (pid <= 0) {
            return srt::Error(srt::Error::InvalidArgument, "the engine process is not running");
        }
        // The wait is taken outside the lock, so a cancellation request from another thread is
        // never held off by a child that is still running.
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        {
            std::lock_guard<std::mutex> lock(handles->mutex);
            if (handles->pid == pid) {
                handles->pid = -1;
            }
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return srt::Error(srt::Error::InvalidArgument, "the engine process was terminated");
    }

    srt::Expected<void> Process::requestCancel() {
        auto *handles = static_cast<Handles *>(m_impl);
        std::lock_guard<std::mutex> lock(handles->mutex);
        if (handles->pid > 0) {
            kill(handles->pid, SIGKILL);
        }
        return srt::Expected<void>();
    }

    srt::Expected<void> Process::terminate() {
        auto *handles = static_cast<Handles *>(m_impl);
        std::lock_guard<std::mutex> lock(handles->mutex);
        if (handles->pid > 0) {
            kill(handles->pid, SIGKILL);
            int status = 0;
            while (waitpid(handles->pid, &status, 0) < 0 && errno == EINTR) {
            }
            handles->pid = -1;
        }
        if (handles->stdinWrite >= 0) {
            ::close(handles->stdinWrite);
            handles->stdinWrite = -1;
        }
        if (handles->stdoutRead >= 0) {
            // Released here as well, because an object reused after a cancelled execution spawns
            // again and would otherwise overwrite the old read end without closing it.
            ::close(handles->stdoutRead);
            handles->stdoutRead = -1;
        }
        return srt::Expected<void>();
    }

    bool Process::alive() const noexcept {
        auto *handles = static_cast<const Handles *>(m_impl);
        return handles->pid > 0;
    }

#endif // !_WIN32

    srt::Expected<void> writeWav(const std::filesystem::path &path,
                                 const std::vector<float> &samples, int sampleRate) {
        std::FILE *file = std::fopen(path.string().c_str(), "wb");
        if (file == nullptr) {
            return srt::Error(srt::Error::FileNotOpen, "cannot write " + path.string());
        }
        const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples.size() * sizeof(float));
        const std::uint32_t rate = static_cast<std::uint32_t>(sampleRate);
        struct {
            char riff[4];
            std::uint32_t riffSize;
            char wave[4];
            char fmt[4];
            std::uint32_t fmtSize;
            std::uint16_t format;
            std::uint16_t channels;
            std::uint32_t sampleRate;
            std::uint32_t byteRate;
            std::uint16_t blockAlign;
            std::uint16_t bits;
            char data[4];
            std::uint32_t dataSize;
        } header{};
        std::memcpy(header.riff, "RIFF", 4);
        header.riffSize = 36 + dataBytes;
        std::memcpy(header.wave, "WAVE", 4);
        std::memcpy(header.fmt, "fmt ", 4);
        header.fmtSize = 16;
        header.format = 3; // IEEE float
        header.channels = 1;
        header.sampleRate = rate;
        header.byteRate = rate * sizeof(float);
        header.blockAlign = sizeof(float);
        header.bits = 32;
        std::memcpy(header.data, "data", 4);
        header.dataSize = dataBytes;
        bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1;
        if (ok && !samples.empty()) {
            ok = std::fwrite(samples.data(), sizeof(float), samples.size(), file) ==
                 samples.size();
        }
        std::fclose(file);
        if (!ok) {
            return srt::Error(srt::Error::FileNotOpen, "cannot write " + path.string());
        }
        return srt::Expected<void>();
    }

    srt::Expected<std::filesystem::path> makeScratchDirectory(const std::string &name) {
        static std::atomic<unsigned long long> counter{0};
        const auto unique =
            std::chrono::steady_clock::now().time_since_epoch().count() * 31 +
            counter.fetch_add(1);
        auto root = std::filesystem::temp_directory_path() / name;
        root /= std::to_string(unique);
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error) {
            return srt::Error(srt::Error::FileNotOpen,
                              "cannot create the scratch directory " + root.string() + ": " +
                                  error.message());
        }
        return root;
    }

    void removeScratchDirectory(const std::filesystem::path &path) {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

}
