#ifndef OTTER_API_NOTEAPIL1_H
#define OTTER_API_NOTEAPIL1_H

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

namespace otter::Api::Note::L1 {

    /// Identifies the note transcription contract.
    inline constexpr char API_INTERFACE[] = "org.openvpi.otter.analysis.Note";

    /// Identifies Level 1 of the contract.
    inline constexpr int API_LEVEL = 1;

    /// Describes one transcribed note.
    struct NoteInfo {
        /// MIDI note number.
        int key = 0;

        /// Start time in seconds, on the host's timeline.
        double start = 0;

        /// Duration in seconds.
        double duration = 0;

        /// Confidence in the inclusive range from 0 to 1.
        double confidence = 0;
    };

    /// Describes one note the caller already knows, to condition transcription on.
    ///
    /// This is the alignment path. Supplying it asks the model to keep these boundaries rather
    /// than find its own, which is what a host does when it already has a score, or timings from
    /// an aligner, and wants pitches filled in against them.
    struct KnownNote {
        /// Start time in seconds, on the host's timeline — the same one \c AudioSegment::startTime
        /// and \c NoteInfo::start are on, not an offset into the span.
        ///
        /// Everything this contract states about time is absolute, and this is the field where
        /// the difference is invisible: a host analyzing its first span would see no difference,
        /// and every later span would be placed wrongly with nothing to say so.
        double start = 0;

        /// Duration in seconds.
        double duration = 0;
    };

    /// Describes the capabilities exported by a note transcription contribution.
    ///
    /// This is the contract's \c exports block, read by the host before it prepares any audio.
    /// Its syntax belongs to the interface and level, not to a variant. The machine readable
    /// schema is docs/schemas/note-1-exports.schema.json.
    class NoteSchema : public srt::ContribExports {
    public:
        inline explicit NoteSchema(std::string variant)
            : srt::ContribExports(API_INTERFACE, std::move(variant), API_LEVEL) {
        }

        /// Input sample rate in hertz. The host resamples to this.
        int sampleRate = 0;

        /// Input channel count.
        int channelCount = 1;

        /// Longest span this analyzer accepts in one execution, in seconds. 0 means no limit.
        ///
        /// This one is a hard constraint rather than advice: the host slices, and a model that
        /// cannot encode more than a minute of audio will fail rather than degrade.
        double maxSegmentDuration = 0;

        /// Language identifiers this module recognizes. Empty means it does not distinguish.
        std::vector<std::string> languages;

        /// Whether this module reads \c knownNotes.
        bool supportsKnownNotes = false;

        /// Threshold at which a note boundary is placed.
        Common::L1::Knob boundaryThreshold;

        /// Smallest interval between adjacent boundaries, in seconds.
        Common::L1::Knob boundaryRadius;

        /// Confidence threshold supplied to the model.
        Common::L1::Knob noteThreshold;

        /// Confidence below which a note is dropped from the result.
        Common::L1::Knob notePresenceCutoff;

        /// Number of sampling steps.
        Common::L1::IntKnob steps;
    };

    /// Reads the \c exports block of \a spec as a Note Level 1 schema.
    ///
    /// Every provider of this contract reads its declaration through this one function, so two
    /// variants cannot disagree about what a key means or how a bad value is reported. A variant
    /// then checks the result against what its own models can honor.
    ///
    /// \c sampleRate is required. \c channelCount defaults to 1, \c maxSegmentDuration to no
    /// limit, \c languages to none, \c supportsKnownNotes to false, and a knob that is not
    /// declared is not honored.
    OTTER_EXPORT srt::Expected<std::unique_ptr<NoteSchema>>
        readNoteSchema(const srt::ContribSpec &spec, std::string variant);

    /// Contains runtime options used when creating a note analyzer.
    class NoteRuntimeOptions : public otter::AnalysisRuntimeOptions {
    public:
        inline explicit NoteRuntimeOptions(std::string variant)
            : otter::AnalysisRuntimeOptions(API_INTERFACE, std::move(variant), API_LEVEL) {
        }
    };

    /// Supplies audio, knob values and optional known notes for one transcription execution.
    class NoteStartInput : public srt::TaskStartInput {
    public:
        inline NoteStartInput() : srt::TaskStartInput(API_INTERFACE, API_LEVEL) {
        }

        /// Audio to transcribe.
        Common::L1::AudioSegment audio;

        /// Receives progress in the inclusive range from 0 to 1.
        Common::L1::ProgressCallback progress;

        /// Language identifier. Must be one the module declares.
        std::optional<std::string> language;

        /// Threshold at which a note boundary is placed.
        std::optional<double> boundaryThreshold;

        /// Smallest interval between adjacent boundaries, in seconds.
        std::optional<double> boundaryRadius;

        /// Confidence threshold supplied to the model.
        std::optional<double> noteThreshold;

        /// Confidence below which a note is dropped from the result.
        std::optional<double> notePresenceCutoff;

        /// Number of sampling steps.
        std::optional<int> steps;

        /// Notes already known, in ascending order and not overlapping, on the host's timeline.
        /// Empty means free transcription. A module that does not declare \c supportsKnownNotes
        /// refuses a non-empty list rather than ignoring it.
        std::vector<KnownNote> knownNotes;
    };

    /// Contains the notes transcribed from one span.
    class NoteResult : public srt::TaskResult {
    public:
        inline NoteResult() : srt::TaskResult(API_INTERFACE, API_LEVEL) {
        }

        /// Notes in ascending order of start time, in seconds on the host's timeline.
        std::vector<NoteInfo> notes;
    };

    /// Executes one note transcription model using Level 1 typed payloads.
    class NoteExecutive : public otter::AnalysisExecutive {
    public:
        using AsyncCallback =
            std::function<void(srt::Expected<std::unique_ptr<NoteResult>> result)>;

        /// Executes one note transcription synchronously.
        ///
        /// One execution at a time: a second call while one is in flight is refused. A stop that
        /// lands during the run makes it report an error with state() Canceled.
        srt::Expected<std::unique_ptr<NoteResult>> start(const NoteStartInput &input) {
            return m_task.run(input);
        }

        /// Starts one asynchronous note transcription on a worker thread.
        ///
        /// The callback may start the next execution or destroy this executive.
        srt::Expected<void> startAsync(std::shared_ptr<const NoteStartInput> input,
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
        virtual srt::Expected<std::unique_ptr<NoteResult>> run(const NoteStartInput &input) = 0;

        /// Whether a stop has been requested for the execution in flight.
        bool cancelled() const noexcept {
            return m_task.cancelled();
        }

        explicit NoteExecutive(otter::AnalysisSpec &spec)
            : otter::AnalysisExecutive(spec),
              m_task([this](const NoteStartInput &input) { return run(input); }) {
        }

    private:
        /// Declared last: the body captures this object.
        otter::AnalysisTask<NoteStartInput, NoteResult> m_task;
    };

}

namespace srt {

    template <>
    struct ContribSpecExtensionTraits<otter::AnalysisSpec, otter::Api::Note::L1::NoteExecutive> {
        inline static constexpr char ID[] = "org.openvpi.otter.extension.Note";
    };

}

#endif // OTTER_API_NOTEAPIL1_H
