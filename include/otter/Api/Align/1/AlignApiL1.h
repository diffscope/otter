#ifndef OTTER_API_ALIGN_1_ALIGNAPIL1_H
#define OTTER_API_ALIGN_1_ALIGNAPIL1_H

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

namespace otter::Api::Align::L1 {

    /// Identifies the Align interface contract.
    inline const srt::ContribContract Contract{"org.openvpi.otter.inference.Align", 1};

    /// Written form of the lyrics of a language.
    enum class LyricsForm {
        /// Syllables written in the language's scheme, such as pinyin. A wolf G2P of the same
        /// language and scheme produces this form from ordinary text.
        Scheme,

        /// Ordinary words of the language, which the module converts internally.
        Text,
    };

    /// Describes one language that a module aligns.
    ///
    /// A language code alone specifies neither the form of the lyrics nor the form of the result:
    /// Mandarin can be written as characters or as pinyin, and aligned to more than one phoneme
    /// set. A language is therefore declared with the scheme of its phonemes, using wolf's scheme
    /// names, so that a host can pair an aligner with the linguist of the same language and
    /// scheme. The phonemes are also listed because a scheme does not determine a unique phoneme
    /// set: two dictionaries of the same scheme can split a syllable differently.
    struct LanguageInfo {
        /// ISO 639-3 code.
        std::string language;

        /// The scheme of the phonemes in a result, such as pinyin or arpabet.
        std::string scheme;

        /// The written form of \c StartInput::lyrics for this language.
        LyricsForm lyrics = LyricsForm::Scheme;

        /// Every phoneme label that a result in this language can contain, excluding
        /// \c Schema::silenceLabel and \c Schema::nonSpeechPhonemes.
        std::vector<std::string> phonemes;
    };

    /// Describes one phoneme the aligner placed.
    struct PhoneInfo {
        /// The phoneme, one of the declared \c LanguageInfo::phonemes of the language the
        /// execution ran in, or the label of the silence or non-speech segment it belongs to.
        std::string text;

        /// Start time in seconds, on the host's timeline.
        double start = 0;

        /// Duration in seconds. Always greater than zero.
        double duration = 0;
    };

    /// Describes one word the aligner placed.
    ///
    /// A word is either a word of \c StartInput::lyrics or a word that the module inserted.
    /// Inserted words report silence and detected non-speech sounds to the host: an inserted word
    /// carries the label of the segment it covers, which is the declared \c silenceLabel for
    /// silence or one of the declared \c nonSpeechPhonemes for a breath.
    struct WordInfo {
        /// The word as the caller wrote it, or the module's label for a segment it inserted.
        std::string text;

        /// Start time in seconds, on the host's timeline.
        double start = 0;

        /// Duration in seconds. Always greater than zero.
        double duration = 0;

        /// The phonemes of this word, in order, covering it exactly.
        std::vector<PhoneInfo> phones;
    };

    /// Describes the capabilities exported by a forced alignment contribution.
    ///
    /// This is the contract's \c exports block, read by the host before it prepares any audio.
    /// Its syntax belongs to the interface and level, not to a variant. The machine readable
    /// schema is docs/schemas/align-1-exports.schema.json.
    class Schema : public srt::ContribExports {
    public:
        inline Schema() : srt::ContribExports(Contract) {
        }

        /// Input sample rate in hertz. The host resamples its audio to this rate.
        int sampleRate = 0;

        /// Input channel count.
        int channelCount = 1;

        /// Longest span this analyzer accepts in one execution, in seconds. 0 indicates no limit.
        ///
        /// The limit is a hard constraint rather than a recommendation: an aligner that cannot
        /// encode more than a minute of audio fails instead of degrading, so the host slices the
        /// audio to stay within the limit.
        double maxSegmentDuration = 0;

        /// The languages this module aligns. A language may appear more than once, each time
        /// with a different scheme. A variant that does not distinguish languages declares none.
        std::vector<LanguageInfo> languages;

        /// The language used by an execution that specifies none. Declared if \c languages is
        /// not empty, and always one of the listed languages.
        std::string defaultLanguage;

        /// The labels that this module can report for non-speech sounds that it detects, such as
        /// a breath. Empty if the module detects no non-speech sounds.
        ///
        /// The labels are the module's own names for its classes, because the set of non-speech
        /// classes depends on the training of each aligner.
        std::vector<std::string> nonSpeechPhonemes;

        /// The non-speech labels detected by an execution that specifies none. A subset of
        /// \c nonSpeechPhonemes.
        std::vector<std::string> defaultNonSpeechPhonemes;

        /// The label of a word that represents silence rather than sung content.
        ///
        /// An empty label indicates that the module does not name silence and does not report
        /// silence as a word. A module may not support an empty label, and then rejects the
        /// declaration or the execution instead of returning words that leave part of the span
        /// uncovered. The host handles that rejection as an error.
        std::string silenceLabel;

        /// Probability above which a frame is judged to be a non-speech sound rather than a
        /// phoneme. Used when the module detects non-speech segments itself.
        Common::L1::Knob nonSpeechThreshold;

        /// Shortest non-speech segment reported, in seconds. Shorter segments are dropped.
        Common::L1::Knob nonSpeechMinDuration;

        /// Longest gap between two aligned words that is absorbed into the surrounding words
        /// rather than left as silence, in seconds.
        Common::L1::Knob gapFill;
    };

    /// Reads the \c exports block of \a spec as an Align Level 1 schema.
    ///
    /// Every provider of this contract reads its declaration through this function, so two
    /// variants cannot differ in the interpretation of a key or in the reporting of an invalid
    /// value. A variant then checks the result against the capabilities of its own model.
    ///
    /// \c sampleRate is required. \c channelCount defaults to 1, \c maxSegmentDuration to no
    /// limit, \c languages and \c nonSpeechPhonemes to none, \c silenceLabel to empty, and a knob
    /// that is not declared is not honored. A language must be an ISO 639-3 code, and a scheme
    /// must follow wolf's grammar. Each language and scheme pair must appear at most once, the
    /// phonemes of an entry must not repeat, a default language is required if languages are
    /// declared, and the default non-speech labels must be among the declared labels.
    ///
    /// \return The schema, or an \c InvalidFormat error that describes the first invalid value.
    OTTER_EXPORT srt::Expected<std::unique_ptr<Schema>>
        readAlignSchema(const srt::ContribSpec &spec);

    /// Contains the import options of an Align analysis module, which identify only the contract.
    ///
    /// A host creating an analyzer at top level passes these to
    /// \c srt::InferenceSpec::createInference together with the runtime options.
    class ImportOptions : public otter::AnalysisImportOptions {
    public:
        inline ImportOptions() : otter::AnalysisImportOptions(Contract) {
        }
    };

    /// Contains runtime options used when creating an aligner.
    class RuntimeOptions : public otter::AnalysisRuntimeOptions {
    public:
        inline RuntimeOptions() : otter::AnalysisRuntimeOptions(Contract) {
        }
    };

    /// Supplies audio, the text to align it against, and knob values, for one execution.
    class StartInput : public srt::TaskStartInput {
    public:
        /// Payload type and version of this class. The task interface compares the type() and
        /// version() of an untyped input with these values before it converts the input to this
        /// class.
        inline static const auto &Contract = L1::Contract;

        inline StartInput() : srt::TaskStartInput(Contract.interfaceId(), Contract.level()) {
        }

        /// Audio to align.
        Common::L1::AudioSegment audio;

        /// Receives progress in the inclusive range from 0 to 1.
        Common::L1::ProgressCallback progress;

        /// Language identifier, which the module must declare. If unset, the declared
        /// \c Schema::defaultLanguage applies.
        std::optional<std::string> language;

        /// Scheme of \c language. An unset scheme is accepted if the module declares the language
        /// with a single scheme, and rejected if the module declares several schemes.
        std::optional<std::string> scheme;

        /// The sung text, space separated, in the form that \c LanguageInfo::lyrics specifies for
        /// the language and scheme in use.
        ///
        /// The field is required, and an empty value is rejected: without lyrics there is no
        /// text to align, and returning a transcription instead would implement a different
        /// contract.
        std::string lyrics;

        /// The declared non-speech labels to detect. If empty, the declared
        /// \c Schema::defaultNonSpeechPhonemes apply. An undeclared label is rejected rather
        /// than ignored: the module cannot produce the requested label, and an empty result would
        /// be indistinguishable from a result without detections.
        std::vector<std::string> nonSpeechPhonemes;

        /// Probability above which a frame is judged to be a non-speech sound.
        std::optional<double> nonSpeechThreshold;

        /// Shortest non-speech segment reported, in seconds.
        std::optional<double> nonSpeechMinDuration;

        /// Longest gap absorbed into the surrounding words rather than left as silence, in
        /// seconds.
        std::optional<double> gapFill;
    };

    /// Contains the words aligned over one span.
    class Result : public srt::TaskResult {
    public:
        /// Payload type and version of this class. The task interface compares the type() and
        /// version() of an untyped result with these values before it converts the result to
        /// this class.
        inline static const auto &Contract = L1::Contract;

        inline Result() : srt::TaskResult(Contract.interfaceId(), Contract.level()) {
        }

        /// The language of the execution, and the scheme of its phonemes.
        std::string language;
        std::string scheme;

        /// Words in ascending order of start time, in seconds on the host's timeline.
        ///
        /// The words cover the span without gaps when \c Schema::silenceLabel is not empty:
        /// the first word starts at the start of the audio, each word ends at the start of the
        /// next word, and the last word ends at the end of the audio. A gap that the aligner did
        /// not attribute to a word becomes a word labelled \c Schema::silenceLabel. A module
        /// whose label is empty does not report silence as a word, so its words may leave part of
        /// the span uncovered and the host pads that part from the audio boundaries. A module that
        /// does not support an empty label rejects the declaration or the execution instead of
        /// returning such a result. A host that requires only the sung words filters on that
        /// label rather than reconstructing the gaps.
        std::vector<WordInfo> words;
    };

    /// Executes one forced alignment model using Level 1 typed payloads.
    class Executive : public otter::AnalysisExecutive {
    public:
        using AsyncCallback = std::function<void(srt::Expected<std::unique_ptr<Result>> result)>;

        /// Executes one alignment synchronously.
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

        /// Starts one alignment on a worker thread.
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

    /// Creates the analyzer of a loaded Align module.
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

#endif // OTTER_API_ALIGN_1_ALIGNAPIL1_H
