#ifndef OTTER_ANALYSISEXECUTIVE_H
#define OTTER_ANALYSISEXECUTIVE_H

#include <memory>
#include <string>

#include <synthrt/Core/ContribExecutive.h>
#include <synthrt/Core/ContribSpecExtension.h>
#include <synthrt/Task/ITask.h>

#include <otter/Analysis/AnalysisContrib.h>
#include <otter/otter_global.h>

namespace otter {

    class AnalysisExecutive;

    /// Runtime options supplied when an analyzer is created.
    class AnalysisRuntimeOptions : public srt::ContribRuntimeOptions {
    public:
        ~AnalysisRuntimeOptions() = default;

    protected:
        using ContribRuntimeOptions::ContribRuntimeOptions;
    };

    /// Checks that \a options were written for the contract \a spec declares.
    ///
    /// An extension is keyed on its contract, but the options object a caller hands it is a
    /// separate value with a contract of its own, and a caller that reaches an F0 extension with
    /// Note options has crossed its wires somewhere. Every provider asks this before it reads the
    /// options, so the mismatch is reported the same way everywhere.
    OTTER_EXPORT srt::Expected<void> checkRuntimeOptions(const AnalysisRuntimeOptions &options,
                                                         const AnalysisSpec &spec);

    /// Adds one analyzer implementation to a loaded AnalysisSpec.
    ///
    /// This is the only way an analysis contribution becomes an executive. The framework's other
    /// route runs through an import binding, and nothing imports an analyzer: the host is the
    /// caller. One extension per contract, keyed by \c ContribSpecExtensionTraits on the contract's
    /// executive type.
    class OTTER_EXPORT AnalysisExtension : public srt::ContribSpecExtension {
    public:
        ~AnalysisExtension();

        /// Returns the analysis declaration extended by this object.
        inline AnalysisSpec &spec() const {
            return *ContribSpecExtension::spec().as<AnalysisSpec>();
        }

        /// Creates this extension's analyzer.
        ///
        /// The implementation must return the concrete executive type its contract requires, so
        /// that the caller may perform the contract defined downcast.
        virtual srt::Expected<std::unique_ptr<AnalysisExecutive>>
            createAnalyzer(const AnalysisRuntimeOptions &runtimeOptions) = 0;

    protected:
        AnalysisExtension(AnalysisSpec &spec, std::string id);
    };

    /// A loaded runtime analyzer of one analysis contribution.
    ///
    /// Each concrete executive exposes one contract-specific execution function. The analyzer owns
    /// whatever sessions its model needs and must be destroyed before the Package containing its
    /// contribution is released.
    class OTTER_EXPORT AnalysisExecutive : public srt::ContribExecutive {
    public:
        explicit AnalysisExecutive(AnalysisSpec &spec);
        ~AnalysisExecutive();

    public:
        inline AnalysisSpec &spec() const {
            return *srt::ContribExecutive::spec().as<AnalysisSpec>();
        }

        /// Returns the state of the current or most recently completed execution.
        virtual srt::ITask::State state() const noexcept = 0;

        /// Requests cancellation of the current execution.
        ///
        /// Cancelling asks the analyzer to stop; it is not a promise about how much of the input
        /// was consumed. A cancelled execution reports \c Canceled rather than a partial result:
        /// its start() returns an error, and state() is what tells a cancellation apart from a
        /// failure, since synthrt's error codes have no value for it.
        virtual srt::Expected<void> stop() = 0;

        /// Waits for the current execution to finish without ending this analyzer's lifetime.
        virtual srt::Expected<void> waitForFinished() = 0;

    protected:
        srt::Expected<void> quit() override;
        srt::Expected<void> wait() override;
    };

}

#endif // OTTER_ANALYSISEXECUTIVE_H
