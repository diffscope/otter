#ifndef OTTER_ANALYSISERROR_H
#define OTTER_ANALYSISERROR_H

#include <system_error>
#include <type_traits>

#include <synthrt/Support/Error.h>

#include <otter/otter_global.h>

namespace otter {

    /// Error conditions that the analysis contracts define in addition to the framework's codes.
    ///
    /// The codes of synthrt belong to the framework, and a library built on synthrt does not
    /// extend that enumeration. The library registers its own error category instead, which is
    /// the category of this enumeration. A caller that receives an srt::Error compares its code()
    /// against these values in the same way as against the framework's codes.
    ///
    /// The code of a failed execution also determines the state of the analyzer: \c Cancelled
    /// leaves the analyzer \c Canceled, and every other code leaves it \c Failed.
    enum class AnalysisError {
        /// The execution was stopped before it produced a result. The condition is reported as
        /// an error because a cancelled execution returns no result, neither partial nor
        /// complete, and state() then returns \c Canceled.
        Cancelled = 1,

        /// No worker thread could be started for an asynchronous execution.
        NoWorker = 2,

        /// The analyzer failed because of an internal fault rather than because of its input:
        /// the execution body threw an exception, the declaration objects that the interpreter
        /// creates at load are missing, or an executive or a result of another contract was
        /// produced.
        Internal = 3,

        /// The model ran but did not produce the outputs that the variant reads: an output is
        /// missing, has an unexpected element type, or does not match the size of the input or of
        /// another output.
        ModelFailed = 4,
    };

    /// Returns the error category that supplies the name and the messages of these codes.
    OTTER_EXPORT const std::error_category &analysisErrorCategory() noexcept;

    /// Returns the std::error_code of \a error in analysisErrorCategory().
    inline std::error_code make_error_code(AnalysisError error) noexcept {
        return {static_cast<int>(error), analysisErrorCategory()};
    }

}

template <>
struct std::is_error_code_enum<otter::AnalysisError> : std::true_type {};

namespace otter {

    /// Returns the error that an execution reports after it detects a stop request.
    ///
    /// Every body reports a cancellation through this function, so that the code and the message
    /// of a cancellation are identical for every variant.
    inline srt::Error cancelledError() {
        return srt::Error(AnalysisError::Cancelled, "the execution was cancelled");
    }

}

#endif // OTTER_ANALYSISERROR_H
