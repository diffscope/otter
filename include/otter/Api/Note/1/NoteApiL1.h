#ifndef OTTER_API_NOTEAPIL1_H
#define OTTER_API_NOTEAPIL1_H

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <synthrt/Core/ContribSpec.h>
#include <synthrt/Core/ContribSpecExtension.h>
#include <synthrt/Task/ITask.h>

#include <otter/Analysis/AnalysisExecutive.h>
#include <otter/Api/Common/1/CommonApiL1.h>

namespace otter::Api::Note::L1 {

    /// Identifies the note transcription contract.
    inline constexpr char API_INTERFACE[] = "org.openvpi.analysis.Note";

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

    /// Contains the interpreted configuration of a note transcription model.
    class NoteConfiguration : public srt::ContribConfiguration {
    public:
        inline explicit NoteConfiguration(std::string variant)
            : srt::ContribConfiguration(API_INTERFACE, std::move(variant), API_LEVEL) {
        }

        /// Path of the audio encoder model.
        std::filesystem::path encoder;

        /// Path of the boundary segmenter model.
        std::filesystem::path segmenter;

        /// Path of the pitch estimator model.
        std::filesystem::path estimator;

        /// Path of the model converting boundaries to durations.
        std::filesystem::path boundaryToDuration;

        /// Path of the model converting known durations to boundaries. Empty when this module
        /// cannot be conditioned on known notes.
        std::filesystem::path durationToBoundary;

        /// Input sample rate in hertz.
        int sampleRate = 0;

        /// Input channel count.
        int channelCount = 1;

        /// Longest span accepted in one execution, in seconds. 0 means no limit.
        double maxSegmentDuration = 0;

        /// Model frame rate in seconds. Used only to convert boundaryRadius into frames.
        double timestep = 0;

        /// Maps language identifiers to the model's own numbering.
        ///
        /// The contract speaks identifiers because a model's internal numbering is its own: two
        /// models need not agree that 1 is the same language.
        std::map<std::string, int> languages;

        /// Language used when the caller supplies none.
        std::string defaultLanguage;

        /// Start of the sampling schedule.
        double scheduleStart = 0;

        /// Knob values used when the caller supplies none.
        int defaultSteps = 8;
        double defaultBoundaryThreshold = 0;
        double defaultBoundaryRadius = 0;
        double defaultNoteThreshold = 0;
        double defaultNotePresenceCutoff = 0;
    };

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

        /// Executes transcription synchronously.
        virtual srt::Expected<std::unique_ptr<NoteResult>> start(const NoteStartInput &input) = 0;

        /// Starts one asynchronous transcription execution.
        virtual srt::Expected<void> startAsync(std::shared_ptr<const NoteStartInput> input,
                                               AsyncCallback callback) = 0;

    protected:
        using otter::AnalysisExecutive::AnalysisExecutive;
    };

}

namespace srt {

    template <>
    struct ContribSpecExtensionTraits<otter::AnalysisSpec, otter::Api::Note::L1::NoteExecutive> {
        inline static constexpr char ID[] = "org.openvpi.analysis.extension.Note";
    };

}

#endif // OTTER_API_NOTEAPIL1_H
