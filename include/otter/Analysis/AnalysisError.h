#ifndef OTTER_ANALYSISERROR_H
#define OTTER_ANALYSISERROR_H

#include <system_error>
#include <type_traits>

#include <otter/otter_global.h>

namespace otter {

    /// The error conditions the analysis contracts add to the framework's.
    ///
    /// synthrt's own codes are the framework's and a library built on it does not extend that
    /// enum; it registers a category of its own, which is this one. A caller that receives an
    /// srt::Error compares its code() against these the same way it would against the
    /// framework's.
    enum class AnalysisError {
        /// The execution was stopped before it produced a result. Reported as an error because
        /// a cancelled execution returns no result; state() says Canceled as well.
        Cancelled = 1,

        /// No worker thread could be started for an asynchronous execution.
        NoWorker = 2,
    };

    /// The category that gives these codes their name and messages.
    OTTER_EXPORT const std::error_category &analysisErrorCategory() noexcept;

    inline std::error_code make_error_code(AnalysisError error) noexcept {
        return {static_cast<int>(error), analysisErrorCategory()};
    }

}

template <>
struct std::is_error_code_enum<otter::AnalysisError> : std::true_type {};

#endif // OTTER_ANALYSISERROR_H
