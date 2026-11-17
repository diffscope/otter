#ifndef OTTER_GGMLUTIL_ENGINEPIPE_H
#define OTTER_GGMLUTIL_ENGINEPIPE_H

#include <cstddef>
#include <string>

#include <stdcorelib/support/popen.h>

/// The pipes of an engine process, a stdc::Popen that a ggml provider owns.
///
/// stdc::Popen synchronizes none of its member functions, so a provider serializes its calls of
/// them under a lock of its own. The streams of the pipes are separate objects, which a provider
/// reads and writes outside that lock, so that a stop from another thread can kill the engine and
/// end a read or a write that the engine no longer serves.
///
/// The functions below go through the FILE of a stream rather than the stream itself. The stream
/// refills its buffer with one fread() of four kilobytes, which on a pipe returns only once the
/// whole count has arrived or the pipe has closed, so a line shorter than that never reaches the
/// caller while the engine waits for the next request.
namespace otter::ggml {

    /// Sets what every engine process of these providers starts with: on Windows, no console
    /// window, which a host without a console would otherwise open for the engine.
    void prepareEngine(stdc::Popen &process);

    /// Writes \a size bytes to the standard input of an engine and flushes them.
    ///
    /// \return Whether every byte was written. A write to an engine that has exited fails instead
    ///         of raising SIGPIPE, whose default action would end the host.
    bool writeToEngine(stdc::Popen::Stream &input, const void *data, std::size_t size);

    /// Reads one line of the standard output of an engine into \a line, without its newline.
    ///
    /// \return Whether a line was read. An engine that exits before the newline fails the read.
    bool readLineFromEngine(stdc::Popen::Stream &output, std::string &line);

}

#endif // OTTER_GGMLUTIL_ENGINEPIPE_H
