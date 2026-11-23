#ifndef OTTER_API_NOTE_1_NOTEAPIL1_H
#define OTTER_API_NOTE_1_NOTEAPIL1_H

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

namespace otter::Api::Note::L1 {

    /// Identifies the Note interface contract.
    inline const srt::ContribContract Contract{"org.openvpi.otter.inference.Note", 1};

    /// Describes one transcribed note.
    struct Info {
        /// MIDI note number.
        int key = 0;

        /// Start time in seconds, on the host's timeline.
        ///
        /// The time is the onset of the sung pitch. For a syllable that begins with a consonant,
        /// the time is the vowel onset rather than the syllable onset. A host that places lyrics
        /// on the notes therefore anchors the vowel of each syllable at this time and places its
        /// consonant before it.
        double start = 0;

        /// Duration in seconds.
        double duration = 0;

        /// Confidence in the inclusive range from 0 to 1.
        double confidence = 0;
    };

    /// Describes one note known to the caller, on which transcription is conditioned.
    ///
    /// Known notes select the alignment path: the model keeps these boundaries instead of
    /// detecting its own. A host supplies known notes if it already has a score, or timings from
    /// an aligner, and requires the pitches for those boundaries.
    struct KnownNote {
        /// Start time in seconds on the host's timeline, which is the timeline of
        /// \c AudioSegment::startTime and \c Info::start, not an offset into the span.
        ///
        /// Every time in this contract is absolute. For this field, confusing an offset with an
        /// absolute time is not detectable on a span that starts at 0, and every later span would
        /// be misplaced without any diagnostic.
        double start = 0;

        /// Duration in seconds.
        double duration = 0;
    };

    /// Describes the capabilities exported by a note transcription contribution.
    ///
    /// This is the contract's \c exports block, read by the host before it prepares any audio.
    /// Its syntax belongs to the interface and level, not to a variant. The machine readable
    /// schema is docs/schemas/note-1-exports.schema.json.
    class Exports : public srt::ContribExports {
    public:
        inline Exports() : srt::ContribExports(Contract) {
        }

        /// Input sample rate in hertz. The host resamples its audio to this rate.
        int sampleRate = 0;

        /// Input channel count.
        int channelCount = 1;

        /// Longest span this analyzer accepts in one execution, in seconds. 0 indicates no limit.
        ///
        /// The limit is a hard constraint rather than a recommendation: the host slices the audio,
        /// because a model that cannot encode more than a minute of audio fails instead of
        /// degrading.
        double maxSegmentDuration = 0;

        /// Language identifiers this module supports, as ISO 639-3 codes. Empty indicates that the
        /// module does not distinguish languages.
        std::vector<std::string> languages;

        /// The language used by an execution that specifies none. Declared if \c languages is not
        /// empty, and always one of the listed languages.
        std::string defaultLanguage;

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

        /// Reads the \c exports block of \a spec as a Note Level 1 schema.
        ///
        /// Every provider of this contract reads its declaration through this function, so two
        /// variants cannot differ in the interpretation of a key or in the reporting of an invalid
        /// value. A variant then checks the result against the capabilities of its own models.
        ///
        /// \c sampleRate is required. \c channelCount defaults to 1, \c maxSegmentDuration to no
        /// limit, \c languages to none, \c supportsKnownNotes to false, and a knob that is not
        /// declared is not honored.
        ///
        /// \return The schema, or an \c InvalidFormat error that describes the first invalid value.
        OTTER_EXPORT static srt::Expected<std::unique_ptr<Exports>>
            read(const srt::ContribSpec &spec);
    };

    /// Contains the import options of a Note analysis module, which identify only the contract.
    ///
    /// A host creating an analyzer at top level passes these to
    /// \c srt::InferenceSpec::createInference together with the runtime options.
    class ImportOptions : public otter::AnalysisImportOptions {
    public:
        inline ImportOptions() : otter::AnalysisImportOptions(Contract) {
        }
    };

    /// Contains runtime options used when creating a note analyzer.
    class RuntimeOptions : public otter::AnalysisRuntimeOptions {
    public:
        inline RuntimeOptions() : otter::AnalysisRuntimeOptions(Contract) {
        }
    };

    /// Supplies audio, knob values and optional known notes for one transcription execution.
    class StartInput : public srt::TaskStartInput {
    public:
        /// Payload type and version of this class. The task interface compares the type() and
        /// version() of an untyped input with these values before it converts the input to this
        /// class.
        inline static const auto &Contract = L1::Contract;

        inline StartInput() : srt::TaskStartInput(Contract) {
        }

        /// Audio to transcribe.
        Common::L1::AudioSegment audio;

        /// Receives progress in the inclusive range from 0 to 1.
        Common::L1::ProgressCallback progress;

        /// Language identifier, which must be one of the languages that the module declares.
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
        /// An empty list selects free transcription. A module that does not declare
        /// \c supportsKnownNotes rejects a non-empty list rather than ignoring it.
        std::vector<KnownNote> knownNotes;
    };

    /// Contains the notes transcribed from one span.
    class Result : public srt::TaskResult {
    public:
        /// Payload type and version of this class. The task interface compares the type() and
        /// version() of an untyped result with these values before it converts the result to
        /// this class.
        inline static const auto &Contract = L1::Contract;

        inline Result() : srt::TaskResult(Contract) {
        }

        /// Notes in ascending order of start time, in seconds on the host's timeline.
        std::vector<Info> notes;
    };

    /// Executes one note transcription model using Level 1 typed payloads.
    class Executive : public otter::AnalysisExecutive {
    public:
        using AsyncCallback = std::function<void(srt::Expected<std::unique_ptr<Result>> result)>;

        /// Executes one note transcription synchronously.
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

        /// Starts one asynchronous note transcription on a worker thread.
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

        /// Creates the analyzer of a loaded Note module.
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
        static srt::Expected<std::unique_ptr<Executive>> create(srt::InferenceSpec &spec) {
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


}

#endif // OTTER_API_NOTE_1_NOTEAPIL1_H
