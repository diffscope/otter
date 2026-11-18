#ifndef OTTER_API_F0_1_F0APIL1_H
#define OTTER_API_F0_1_F0APIL1_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <synthrt/Core/ContribSpec.h>
#include <synthrt/Support/Expected.h>
#include <synthrt/Task/ITask.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/AnalysisExecutive.h>
#include <otter/Analysis/Provider/AnalysisTask.h>
#include <otter/Api/Common/1/CommonApiL1.h>
#include <otter/otter_global.h>

namespace otter::Api::F0::L1 {

    /// Identifies the F0 interface contract.
    inline const srt::ContribContract Contract{"org.openvpi.otter.inference.F0", 1};

    /// Describes the capabilities exported by an F0 analysis contribution.
    ///
    /// This is the contract's \c exports block, read by the host before it prepares any audio.
    /// Its syntax belongs to the interface and level, not to a variant, so every variant of this
    /// contract declares the same keys and a host reads them the same way. The machine readable
    /// schema is docs/schemas/f0-1-exports.schema.json.
    class Schema : public srt::ContribExports {
    public:
        inline Schema() : srt::ContribExports(Contract) {
        }

        /// Input sample rate in hertz. The host resamples its audio to this rate.
        int sampleRate = 0;

        /// Input channel count.
        int channelCount = 1;

        /// Time between adjacent output frames in seconds.
        double interval = 0;

        /// Longest span this analyzer accepts in one execution, in seconds. 0 indicates no limit.
        double maxSegmentDuration = 0;

        /// Threshold above which a frame is judged voiced.
        Common::L1::Knob voicingThreshold;

        /// Whether to interpolate the curve across unvoiced frames instead of leaving it at zero.
        Common::L1::FlagKnob interpolateUnvoiced;
    };

    /// Reads the \c exports block of \a spec as an F0 Level 1 schema.
    ///
    /// Every provider of this contract reads its declaration through this function, so two
    /// variants cannot differ in the interpretation of a key or in the reporting of an invalid
    /// value. A variant then checks the result against the capabilities of its own models.
    ///
    /// \c sampleRate and \c interval are required. \c channelCount defaults to 1,
    /// \c maxSegmentDuration to no limit, and a knob that is not declared is not honored.
    ///
    /// \return The schema, or an \c InvalidFormat error that describes the first invalid value.
    OTTER_EXPORT srt::Expected<std::unique_ptr<Schema>> readF0Schema(const srt::ContribSpec &spec);

    /// Contains the import options of an F0 analysis module, which identify only the contract.
    ///
    /// A host creating an analyzer at top level passes these to
    /// \c srt::InferenceSpec::createInference together with the runtime options.
    class ImportOptions : public otter::AnalysisImportOptions {
    public:
        inline ImportOptions() : otter::AnalysisImportOptions(Contract) {
        }
    };

    /// Contains runtime options used when creating an F0 analyzer.
    class RuntimeOptions : public otter::AnalysisRuntimeOptions {
    public:
        inline RuntimeOptions() : otter::AnalysisRuntimeOptions(Contract) {
        }
    };

    /// Supplies audio and knob values for one F0 analysis execution.
    ///
    /// Every knob is optional. An empty knob selects the default of the module, so that a host
    /// written before the introduction of a knob continues to work unchanged afterwards.
    class StartInput : public srt::TaskStartInput {
    public:
        /// Payload type and version of this class. The task interface compares the type() and
        /// version() of an untyped input with these values before it converts the input to this
        /// class.
        inline static const auto &Contract = L1::Contract;

        inline StartInput() : srt::TaskStartInput(Contract.interfaceId(), Contract.level()) {
        }

        /// Audio to analyze.
        Common::L1::AudioSegment audio;

        /// Receives progress in the inclusive range from 0 to 1.
        Common::L1::ProgressCallback progress;

        /// Threshold above which a frame is judged voiced.
        std::optional<double> voicingThreshold;

        /// Whether to interpolate the curve across unvoiced frames.
        std::optional<bool> interpolateUnvoiced;
    };

    /// Contains the curve produced for one span.
    class Result : public srt::TaskResult {
    public:
        /// Payload type and version of this class. The task interface compares the type() and
        /// version() of an untyped result with these values before it converts the result to
        /// this class.
        inline static const auto &Contract = L1::Contract;

        inline Result() : srt::TaskResult(Contract.interfaceId(), Contract.level()) {
        }

        /// Time of the first frame in seconds, equal to the input's startTime.
        double startTime = 0;

        /// Time between adjacent frames in seconds.
        double interval = 0;

        /// Fundamental frequency in hertz. The value of an unvoiced frame depends on
        /// interpolateUnvoiced: the interpolated value if interpolation is enabled, and zero
        /// otherwise.
        std::vector<float> f0;

        /// Voicing flags, one entry per frame of \c f0: 1 marks a voiced frame.
        ///
        /// The name states the meaning of the value. The corresponding flag of the refactor line
        /// was named \c uv although it was documented as true for a voiced frame. The name of
        /// this member avoids that mismatch between name and meaning.
        std::vector<uint8_t> voiced;
    };

    /// Executes one F0 analysis model using Level 1 typed payloads.
    ///
    /// A provider implementing this contract must create executives derived from this class.
    class Executive : public otter::AnalysisExecutive {
    public:
        using AsyncCallback = std::function<void(srt::Expected<std::unique_ptr<Result>> result)>;

        /// Executes one F0 analysis synchronously.
        ///
        /// Only one execution runs at a time, and a call during another execution is rejected. A
        /// stop request that arrives during the execution causes an error, and state() then
        /// returns \c Canceled.
        ///
        /// \return The result of the execution, or the error that the execution reports. The
        /// error code is \c AnalysisError::Cancelled if the execution detected a stop request.
        srt::Expected<std::unique_ptr<Result>> start(const StartInput &input) {
            return m_task.run(input);
        }

        /// Starts one asynchronous F0 analysis on a worker thread.
        ///
        /// The callback may start the next execution or destroy this executive.
        ///
        /// \return An empty value if the execution started, or the error that prevented the
        /// start.
        srt::Expected<void> startAsync(std::shared_ptr<const StartInput> input,
                                       AsyncCallback callback) {
            return m_task.runAsync(std::move(input), std::move(callback));
        }

        /// Returns the state of the current or most recently completed execution.
        srt::ITask::State state() const noexcept override {
            return m_task.state();
        }

    protected:
        /// Requests cancellation of the current execution. A provider whose model session blocks
        /// overrides this function, calls it, and then also cancels the session.
        srt::Expected<void> quit() override {
            return m_task.stop();
        }

        /// Waits for the current execution and, for an asynchronous execution, its callback. A
        /// provider with model sessions overrides this function, calls it, and then also waits
        /// for the sessions.
        srt::Expected<void> wait() override {
            return m_task.waitForFinished();
        }

        /// Runs the body of one execution. It is the only function that a provider implements.
        ///
        /// The function runs on the caller's thread for start() and on a worker thread for
        /// startAsync(). It polls cancelled() at the granularity at which it can stop, and returns
        /// cancelledError() if a stop was requested. The provider's destructor must call stop()
        /// and waitForFinished() before it destroys any object that the body reads.
        virtual srt::Expected<std::unique_ptr<Result>> run(const StartInput &input) = 0;

        /// Returns whether a stop has been requested for the current execution.
        bool cancelled() const noexcept {
            return m_task.cancelled();
        }

        explicit Executive(srt::InferenceSpec &spec)
            : otter::AnalysisExecutive(spec),
              m_task([this](const StartInput &input) { return run(input); }) {
        }

    private:
        // Declared last because the body captures this object.
        otter::AnalysisTask<StartInput, Result> m_task;
    };

    /// Creates the analyzer of a loaded F0 module.
    ///
    /// A host creates an analyzer through this function without importing the module first. The
    /// options identify only the interface contract and its level.
    ///
    /// The type of the executive is checked rather than assumed, because a third-party variant
    /// may supply the interpreter. An executive of another contract is rejected with
    /// \c AnalysisError::ContractViolation instead of being converted.
    ///
    /// \return The analyzer, the error of \c srt::InferenceSpec::createInference, or an
    /// \c AnalysisError::ContractViolation error if the executive is not an Executive.
    inline srt::Expected<std::unique_ptr<Executive>> createAnalyzer(srt::InferenceSpec &spec) {
        auto made = spec.createInference(ImportOptions(), RuntimeOptions());
        if (!made) {
            return made.takeError();
        }
        auto executive = made.take();
        auto typed = dynamic_cast<Executive *>(executive.get());
        if (typed == nullptr) {
            return srt::Error(AnalysisError::ContractViolation,
                              "the interpreter of " + spec.triplet().variant() +
                                  " created an executive that is not an Executive");
        }
        (void) executive.release();
        return std::unique_ptr<Executive>(typed);
    }

}

#endif // OTTER_API_F0_1_F0APIL1_H
