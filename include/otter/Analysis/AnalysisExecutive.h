#ifndef OTTER_ANALYSISEXECUTIVE_H
#define OTTER_ANALYSISEXECUTIVE_H

#include <memory>
#include <string>

#include <synthrt/SVS/InferenceContrib.h>
#include <synthrt/SVS/InferenceExecutive.h>
#include <synthrt/Task/ITask.h>

#include <otter/otter_global.h>

namespace otter {

    /// Import options of an analysis contract.
    ///
    /// Analysis contracts define no import options; the options of each contract therefore identify
    /// only that contract. A host that creates an analyzer at top level passes the contract's
    /// subclass to \c srt::InferenceSpec::createInference, and a module that imports an analyzer
    /// declares an empty \c options object.
    class AnalysisImportOptions : public srt::ContribImportOptions {
    public:
        ~AnalysisImportOptions() = default;

    protected:
        using ContribImportOptions::ContribImportOptions;
    };

    /// Runtime options supplied when an analyzer is created.
    class AnalysisRuntimeOptions : public srt::InferenceRuntimeOptions {
    public:
        ~AnalysisRuntimeOptions() = default;

    protected:
        using InferenceRuntimeOptions::InferenceRuntimeOptions;
    };

    /// A loaded runtime analyzer of one analysis module.
    ///
    /// An analysis module is an inference module whose interface is one of otter's contracts. Each
    /// concrete executive exposes one contract-specific execution function. The analyzer owns
    /// the sessions of its model and must be destroyed before the Package that contains its
    /// module is released.
    ///
    /// Every shipped executive holds references to the exports and the configuration of its spec
    /// rather than copies. These references rely on the rule above: the spec owns both objects,
    /// the Package owns the spec, and the executive is destroyed before the Package is released.
    /// An executive that outlived its Package would read freed declaration objects; a host
    /// therefore must not keep an executive beyond the lifetime of the handle of its Package.
    class OTTER_EXPORT AnalysisExecutive : public srt::InferenceExecutive {
    public:
        explicit AnalysisExecutive(srt::InferenceSpec &spec);
        ~AnalysisExecutive();

    public:
        /// Returns the state of the current or most recently completed execution.
        srt::ITask::State state() const noexcept override = 0;

        /// Requests cancellation of the current execution.
        ///
        /// A cancellation request does not specify how much of the input is consumed. An
        /// execution that detects the request returns no result, neither partial nor complete:
        /// its start() returns an error whose code is \c AnalysisError::Cancelled, the callback of
        /// startAsync() receives the same error, and state() then returns \c Canceled. An
        /// execution that had already produced its result, or failed for another reason, when the
        /// request arrived keeps that outcome and the corresponding state.
        ///
        /// This behavior differs deliberately from wolf's linguists. A stopped conversion of a
        /// linguist returns the words completed before the stop, with state() \c Canceled, and
        /// those words are usable without further processing. A partial analysis would be a
        /// curve, a note list or an alignment that covers an unknown part of the span and has the
        /// same shape as a complete result over shorter audio; a partial analysis is therefore not
        /// returned.
        srt::Expected<void> stop() override = 0;

        /// Waits for the current execution to finish without ending this analyzer's lifetime.
        srt::Expected<void> waitForFinished() override = 0;
    };

}

#endif // OTTER_ANALYSISEXECUTIVE_H
