#ifndef OTTER_API_F0APIL1_H
#define OTTER_API_F0APIL1_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <synthrt/Core/ContribSpec.h>
#include <synthrt/Core/ContribSpecExtension.h>
#include <synthrt/Support/Expected.h>
#include <synthrt/Task/ITask.h>

#include <otter/Analysis/AnalysisExecutive.h>
#include <otter/Analysis/AnalysisTask.h>
#include <otter/Api/Common/1/CommonApiL1.h>
#include <otter/otter_global.h>

namespace otter::Api::F0::L1 {

    /// Identifies the fundamental frequency analysis contract.
    inline constexpr char API_INTERFACE[] = "org.openvpi.otter.analysis.F0";

    /// Identifies Level 1 of the contract.
    inline constexpr int API_LEVEL = 1;

    /// Describes the capabilities exported by an F0 analysis contribution.
    ///
    /// This is the contract's \c exports block, read by the host before it prepares any audio.
    /// Its syntax belongs to the interface and level, not to a variant, so every variant of this
    /// contract declares the same keys and a host reads them the same way. The machine readable
    /// schema is docs/schemas/f0-1-exports.schema.json.
    class F0Schema : public srt::ContribExports {
    public:
        inline explicit F0Schema(std::string variant)
            : srt::ContribExports(API_INTERFACE, std::move(variant), API_LEVEL) {
        }

        /// Input sample rate in hertz. The host resamples to this.
        int sampleRate = 0;

        /// Input channel count.
        int channelCount = 1;

        /// Time between adjacent output frames in seconds.
        double interval = 0;

        /// Longest span this analyzer accepts in one execution, in seconds. 0 means no limit.
        double maxSegmentDuration = 0;

        /// Threshold above which a frame is judged voiced.
        Common::L1::Knob voicingThreshold;

        /// Whether to interpolate the curve across unvoiced frames instead of leaving it at zero.
        Common::L1::FlagKnob interpolateUnvoiced;
    };

    /// Reads the \c exports block of \a spec as an F0 Level 1 schema.
    ///
    /// Every provider of this contract reads its declaration through this one function, so two
    /// variants cannot disagree about what a key means or how a bad value is reported. A variant
    /// then checks the result against what its own models can honor. \a variant is recorded in
    /// the schema so a host can tell which implementation answers.
    ///
    /// \c sampleRate and \c interval are required. \c channelCount defaults to 1,
    /// \c maxSegmentDuration to no limit, and a knob that is not declared is not honored.
    OTTER_EXPORT srt::Expected<std::unique_ptr<F0Schema>> readF0Schema(const srt::ContribSpec &spec,
                                                                       std::string variant);

    /// Contains runtime options used when creating an F0 analyzer.
    class F0RuntimeOptions : public otter::AnalysisRuntimeOptions {
    public:
        inline explicit F0RuntimeOptions(std::string variant)
            : otter::AnalysisRuntimeOptions(API_INTERFACE, std::move(variant), API_LEVEL) {
        }
    };

    /// Supplies audio and knob values for one F0 analysis execution.
    ///
    /// Every knob is optional. Leaving one empty selects the module's own default, which is what
    /// keeps a host that predates a knob working unchanged once the knob exists.
    class F0StartInput : public srt::TaskStartInput {
    public:
        inline F0StartInput() : srt::TaskStartInput(API_INTERFACE, API_LEVEL) {
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
    class F0Result : public srt::TaskResult {
    public:
        inline F0Result() : srt::TaskResult(API_INTERFACE, API_LEVEL) {
        }

        /// Time of the first frame in seconds, equal to the input's startTime.
        double startTime = 0;

        /// Time between adjacent frames in seconds.
        double interval = 0;

        /// Fundamental frequency in hertz. What unvoiced frames carry depends on
        /// interpolateUnvoiced: the interpolated value when it is on, zero when it is off.
        std::vector<float> f0;

        /// One entry per frame of \c f0; 1 marks a voiced frame.
        ///
        /// Named for what it means. The flag the refactor line carried was called \c uv while
        /// documented as true-means-voiced, which is the kind of disagreement that survives review
        /// and then costs an afternoon.
        std::vector<uint8_t> voiced;
    };

    /// Executes one F0 analysis model using Level 1 typed payloads.
    ///
    /// A provider implementing this contract must create executives derived from this class.
    class F0Executive : public otter::AnalysisExecutive {
    public:
        using AsyncCallback = std::function<void(srt::Expected<std::unique_ptr<F0Result>> result)>;

        /// Executes one F0 analysis synchronously.
        ///
        /// One execution at a time: a second call while one is in flight is refused. A stop that
        /// lands during the run makes it report an error with state() Canceled.
        srt::Expected<std::unique_ptr<F0Result>> start(const F0StartInput &input) {
            return m_task.run(input);
        }

        /// Starts one asynchronous F0 analysis on a worker thread.
        ///
        /// The callback may start the next execution or destroy this executive.
        srt::Expected<void> startAsync(std::shared_ptr<const F0StartInput> input,
                                       AsyncCallback callback) {
            return m_task.runAsync(std::move(input), std::move(callback));
        }

        srt::ITask::State state() const noexcept override {
            return m_task.state();
        }

        /// Requests cancellation. A provider whose model session blocks overrides this to tell
        /// the session too, after calling this.
        srt::Expected<void> stop() override {
            return m_task.stop();
        }

        /// Waits for the current execution and, for an asynchronous one, its callback. A
        /// provider with model sessions overrides this to wait on them too, after calling this.
        srt::Expected<void> waitForFinished() override {
            return m_task.waitForFinished();
        }

    protected:
        /// The body of one execution, which is the one thing a provider writes.
        ///
        /// Runs on the caller's thread for start() and on a worker for startAsync(). It polls
        /// cancelled() at whatever granularity it can afford to stop at and returns an error when
        /// it is set. The provider's destructor must call stop() and waitForFinished() before it
        /// destroys anything the body reads.
        virtual srt::Expected<std::unique_ptr<F0Result>> run(const F0StartInput &input) = 0;

        /// Whether a stop has been requested for the execution in flight.
        bool cancelled() const noexcept {
            return m_task.cancelled();
        }

        explicit F0Executive(otter::AnalysisSpec &spec)
            : otter::AnalysisExecutive(spec),
              m_task([this](const F0StartInput &input) { return run(input); }) {
        }

    private:
        /// Declared last: the body captures this object.
        otter::AnalysisTask<F0StartInput, F0Result> m_task;
    };

}

namespace srt {

    template <>
    struct ContribSpecExtensionTraits<otter::AnalysisSpec, otter::Api::F0::L1::F0Executive> {
        inline static constexpr char ID[] = "org.openvpi.otter.extension.F0";
    };

}

#endif // OTTER_API_F0APIL1_H
