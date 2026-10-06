#ifndef OTTER_CLIPROCESS_H
#define OTTER_CLIPROCESS_H

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <synthrt/Support/Expected.h>

/// Code shared by the providers that drive an external command line tool: one child process with
/// redirected standard streams, the mono float32 WAV file an execution hands its engine, and the
/// scratch directory the files of one execution live in.
///
/// This namespace is implemented by a private static library of this tree that each external
/// provider links. It cannot reside in the otter library because the otter library does not spawn
/// processes.
namespace otter::cli {

    /// One child process.
    ///
    /// The process is spawned with its standard input and standard output as pipes this object
    /// holds; its standard error goes to the parent's standard error, where the engine's own
    /// progress notes belong. The destructor terminates a process that is still running, so a
    /// destroyed executive never leaves an engine behind.
    class Process {
    public:
        Process();
        ~Process();
        Process(Process &&) noexcept;
        Process &operator=(Process &&) noexcept;
        Process(const Process &) = delete;
        Process &operator=(const Process &) = delete;

        /// Spawns \a commandLine, whose first entry is the program and whose further entries are
        /// the arguments, quoted by the platform's rules. The entries must not contain embedded
        /// newlines, which no path or argument of these providers does.
        ///
        /// \return An empty value, or the system error that prevented the spawn.
        srt::Expected<void> start(const std::vector<std::string> &commandLine);

        /// Writes \a size bytes to the standard input of the process. A write can block until the
        /// process has consumed its input; a process that has exited fails the write.
        srt::Expected<void> write(const void *data, std::size_t size);

        /// Reads the standard output of the process up to and including the next newline, which
        /// the engines put at the end of every response. A read blocks until the line arrives or
        /// the process exits; a process that exits without another line fails the read.
        srt::Expected<std::string> readLine();

        /// Waits for the process to exit and returns its exit code. A code other than zero is
        /// reported through this value rather than an error, because a tool that refused its
        /// input did run.
        srt::Expected<int> wait();

        /// Requests that the process die, without waiting for it and without releasing anything.
        ///
        /// This is the one call another thread may make while the owning thread sits in write(),
        /// readLine() or wait(): it takes only the object's own lock, kills the child, and leaves
        /// every handle and every descriptor for the owning thread to reap, as the contract of a
        /// cancellation requires. A process that has already exited reports an empty value.
        srt::Expected<void> requestCancel();

        /// Terminates the process. A process that has already exited reports an empty value,
        /// which makes this usable as the unconditional cancellation of a stopped execution.
        srt::Expected<void> terminate();

        /// Whether the process has been spawned and has not been waited out or terminated yet.
        bool alive() const noexcept;

    private:
        void close();
        void *m_impl = nullptr;
    };

    /// Writes \a samples, one channel, as a float32 WAV file at \a sampleRate.
    ///
    /// The engines read WAV files of their own, so the file carries the samples through unchanged.
    srt::Expected<void> writeWav(const std::filesystem::path &path,
                                 const std::vector<float> &samples, int sampleRate);

    /// Creates a scratch directory of one execution, named \a name under the system's temporary
    /// directory and unique to this run of the process. The caller removes it, best effort, when
    /// the execution is over.
    srt::Expected<std::filesystem::path> makeScratchDirectory(const std::string &name);

    /// Removes a scratch directory and everything it still holds, reporting nothing: the leftovers
    /// of one execution are never worth a failed one.
    void removeScratchDirectory(const std::filesystem::path &path);

}

#endif // OTTER_CLIPROCESS_H
