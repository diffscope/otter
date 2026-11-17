#ifndef OTTER_GGMLUTIL_ENGINEFILES_H
#define OTTER_GGMLUTIL_ENGINEFILES_H

#include <filesystem>
#include <string>
#include <vector>

#include <synthrt/Support/Expected.h>

/// The files that the providers driving a ggml engine hand the engine: the mono float32 WAV file of
/// an execution and the scratch directory the files of one execution live in.
///
/// This namespace is implemented by the private static library ggmlutil of this tree, which each
/// ggml provider links. The child process of the engine is a stdc::Popen of each provider.
namespace otter::ggml {

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

#endif // OTTER_GGMLUTIL_ENGINEFILES_H
