// The note transcription provider, ported from the GAME extractor.
//
// Four models run in sequence: an encoder converts audio into features, a segmenter places note
// boundaries, a third model converts boundaries into durations, and an estimator assigns each
// note a pitch and a confidence. An optional fifth model converts durations known to the caller
// into boundaries; this conversion conditions the transcription on an existing score.
//
// This port differs from the original implementation in two respects. Time is measured in seconds
// throughout instead of ticks at a fixed tempo; therefore no code here depends on a tempo, and a
// score with tempo changes is placed correctly. In addition, the fifth model is connected: the
// released weights always contained it, but the earlier port did not open its session, so the
// segmenter received zeros as known boundaries.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <stdcorelib/plugin/plugin.h>

#include <synthrt/SVS/InferenceInterpreterPlugin.h>
#include <synthrt/Support/Expected.h>
#include <synthrt/Support/JSON.h>

#include <dsinfer/Core/Tensor.h>
#include <dsinfer/Inference/InferenceSession.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/AnalysisInput.h>
#include <otter/Analysis/AnalysisInterpreter.h>
#include <otter/Api/Note/1/NoteApiL1.h>
#include <otter/Support/KnownNotes.h>
#include <otter/Support/ManifestValues.h>

#include "OnnxSupport.h"

namespace NoteApi = otter::Api::Note::L1;
namespace CommonApi = otter::Api::Common::L1;

namespace {

    constexpr char VARIANT[] = "game";

    constexpr int MODEL_SAMPLE_RATE = 44100;

    /// Knob values used if the declaration does not honor the corresponding knob.
    constexpr int FALLBACK_STEPS = 8;
    constexpr double FALLBACK_BOUNDARY_THRESHOLD = 0.2;
    constexpr double FALLBACK_BOUNDARY_RADIUS = 0.02;
    constexpr double FALLBACK_NOTE_THRESHOLD = 0.2;
    constexpr double FALLBACK_NOTE_PRESENCE_CUTOFF = 0.5;

    /// The tensor names of the five exported graphs. They form the contract between this variant
    /// and its models and are defined once here instead of at each place where a tensor is built
    /// or read.
    namespace tensors {
        /// encoder: waveform, duration -> x_seg, x_est, maskT
        constexpr char WAVEFORM[] = "waveform";
        constexpr char DURATION[] = "duration";
        constexpr char X_SEG[] = "x_seg";
        constexpr char X_EST[] = "x_est";
        constexpr char MASK_T[] = "maskT";

        /// segmenter: x_seg, maskT, known_boundaries, prev_boundaries, language, threshold,
        /// radius, t -> boundaries
        constexpr char KNOWN_BOUNDARIES[] = "known_boundaries";
        constexpr char PREV_BOUNDARIES[] = "prev_boundaries";
        constexpr char LANGUAGE[] = "language";
        constexpr char THRESHOLD[] = "threshold";
        constexpr char RADIUS[] = "radius";
        constexpr char SCHEDULE_TIME[] = "t";
        constexpr char BOUNDARIES[] = "boundaries";

        /// bd2dur: boundaries, maskT -> durations, maskN
        constexpr char DURATIONS[] = "durations";
        constexpr char MASK_N[] = "maskN";

        /// estimator: x_est, boundaries, maskT, maskN, threshold -> presence, scores
        constexpr char PRESENCE[] = "presence";
        constexpr char SCORES[] = "scores";

        // dur2bd: durations, maskT -> boundaries
    }

    /// The configuration block of this variant: the model paths and the model wiring, which are
    /// internal to the variant and not visible to the host. The audio format, the language
    /// identifiers and the knobs are contract facts and are declared in exports.
    class GameConfiguration : public srt::ContribConfiguration {
    public:
        GameConfiguration()
            : srt::ContribConfiguration(NoteApi::API_INTERFACE, VARIANT, NoteApi::API_LEVEL) {
        }

        std::filesystem::path encoder;
        std::filesystem::path segmenter;
        std::filesystem::path estimator;
        std::filesystem::path boundaryToDuration;

        /// Empty if this package ships no alignment model.
        std::filesystem::path durationToBoundary;

        /// Model frame rate in seconds. Used only to convert boundaryRadius into frames.
        double timestep = 0.01;

        /// Maps the language identifiers listed in the exports to the model's own numbering.
        ///
        /// The contract uses identifiers because the internal numbering is specific to each model:
        /// the number 1 need not denote the same language in two models.
        std::map<std::string, int> languages;

        /// Start of the sampling schedule.
        double scheduleStart = 0;
    };

    using otter::onnx::TensorPtr;

    /// Returns the sampling schedule: \a steps evenly spaced points from \a start up to, but
    /// excluding, one.
    ///
    /// \return The schedule points, or an empty vector if \a steps is not positive.
    std::vector<float> schedule(double start, int steps) {
        std::vector<float> result;
        if (steps <= 0) {
            return result;
        }
        const auto span = (1.0 - start) / steps;
        result.reserve(static_cast<std::size_t>(steps));
        for (int i = 0; i < steps; ++i) {
            result.push_back(static_cast<float>(start + i * span));
        }
        return result;
    }

    srt::Expected<TensorPtr> floats(const std::vector<std::int64_t> &shape,
                                    stdc::array_view<float> values) {
        auto tensor = ds::Tensor::createFromView<float>(shape, values);
        if (!tensor) {
            return tensor.takeError();
        }
        return TensorPtr(tensor.take());
    }

    srt::Expected<TensorPtr> flags(const std::vector<std::int64_t> &shape,
                                   const std::vector<std::uint8_t> &values) {
        // A bool tensor stores one byte per element, which is the layout that the raw view
        // requires.
        const stdc::array_view<std::byte> raw(reinterpret_cast<const std::byte *>(values.data()),
                                              values.size());
        auto tensor = ds::Tensor::createFromRawView(ds::ITensor::Bool, shape, raw);
        if (!tensor) {
            return tensor.takeError();
        }
        return TensorPtr(tensor.take());
    }

    /// Reads a bool tensor as one byte per element.
    ///
    /// \return One value per element, 0 or 1, or \c AnalysisError::ModelFailed if \a tensor is
    ///         null or not a bool tensor.
    srt::Expected<std::vector<std::uint8_t>> readFlags(const TensorPtr &tensor, const char *what) {
        if (!tensor || tensor->dataType() != ds::ITensor::Bool) {
            return srt::Error(otter::AnalysisError::ModelFailed,
                              std::string(what) + " is not a boolean tensor");
        }
        const auto raw = tensor->rawView();
        std::vector<std::uint8_t> result(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            result[i] = raw[i] == std::byte{0} ? 0 : 1;
        }
        return result;
    }

    /// Reads a confidence that an export may have written as either a number or a flag.
    ///
    /// The shipped GAME model indicates the presence of a note with a boolean; the contract
    /// requires a confidence between zero and one, and a flag is a confidence with two values. The
    /// tensor records its own data type, so both forms are read without a declaration.
    ///
    /// \return One confidence per element, or \c AnalysisError::ModelFailed if \a tensor is
    ///         neither a bool nor a float tensor.
    srt::Expected<std::vector<float>> readConfidences(const TensorPtr &tensor, const char *what) {
        if (tensor && tensor->dataType() == ds::ITensor::Bool) {
            const auto raw = tensor->rawData();
            std::vector<float> result;
            result.reserve(tensor->elementCount());
            for (std::size_t i = 0; i < tensor->elementCount(); ++i) {
                result.push_back(static_cast<unsigned char>(raw[i]) != 0 ? 1.0f : 0.0f);
            }
            return result;
        }
        return otter::onnx::readFloats(tensor, what);
    }

    /// The sessions of one analyzer, one session per model, as the interpreter opens them.
    template <class Session>
    struct Models {
        Session encoder{};
        Session segmenter{};
        Session estimator{};
        Session boundaryToDuration{};

        /// Empty if the package ships no alignment model.
        Session durationToBoundary{};
    };

    using OpenedModels = Models<std::unique_ptr<ds::InferenceSession>>;

    class GameExecutive : public otter::onnx::OnnxExecutive<NoteApi::NoteExecutive> {
    public:
        GameExecutive(srt::InferenceSpec &spec, OpenedModels models,
                      const NoteApi::NoteSchema &schema, const GameConfiguration &configuration)
            : OnnxExecutive(spec), m_schema(schema), m_configuration(configuration) {
            m_models.encoder = own(std::move(models.encoder));
            m_models.segmenter = own(std::move(models.segmenter));
            m_models.estimator = own(std::move(models.estimator));
            m_models.boundaryToDuration = own(std::move(models.boundaryToDuration));
            m_models.durationToBoundary = own(std::move(models.durationToBoundary));
        }

        ~GameExecutive() override {
            shutDown();
        }

    protected:
        srt::Expected<std::unique_ptr<NoteApi::NoteResult>>
            run(const NoteApi::NoteStartInput &input) override {
            const auto &audio = input.audio;
            auto prepared = otter::prepareSamples(audio, m_schema.sampleRate, m_schema.channelCount,
                                                  m_schema.maxSegmentDuration);
            if (!prepared) {
                return prepared.takeError();
            }
            const auto waveform = prepared.take();
            auto language = chooseLanguage(input);
            if (!language) {
                return language.takeError();
            }
            auto chosen = chooseSettings(input);
            if (!chosen) {
                return chosen.takeError();
            }
            const auto settings = chosen.take();

            // Known notes are checked before any model runs, because a rejection after the encoder
            // would waste the encoder's forward pass.
            if (!input.knownNotes.empty()) {
                if (!m_schema.supportsKnownNotes || !m_models.durationToBoundary) {
                    return srt::Error(srt::Error::FeatureNotSupported,
                                      "this model cannot be conditioned on known notes");
                }
                if (auto checked = otter::support::checkKnownNotes(input.knownNotes, audio);
                    !checked) {
                    return checked.takeError();
                }
            }

            const auto report = [&input](double value) {
                if (input.progress) {
                    input.progress(value);
                }
            };
            report(0);

            // 1. Encode.
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }
            auto encoded = encode(waveform, audio.duration());
            if (!encoded) {
                return encoded.takeError();
            }
            const auto &features = encoded->features;
            report(0.3);

            // 2. Known boundaries, if the caller supplied a score to align against.
            std::vector<std::uint8_t> known(encoded->frames.size(), 0);
            if (!input.knownNotes.empty()) {
                auto converted =
                    knownBoundaries(input.knownNotes, audio.startTime, encoded->frames);
                if (!converted) {
                    return converted.takeError();
                }
                known = converted.take();
            }

            // 3. Segment.
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }
            auto segmented = segment(*encoded, known, *language, settings, report);
            if (!segmented) {
                return segmented.takeError();
            }
            const auto boundaries = segmented.take();
            report(0.6);

            // 4. Boundaries to durations.
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }
            auto measured = measure(boundaries, features);
            if (!measured) {
                return measured.takeError();
            }
            report(0.8);

            // 5. Estimate pitch.
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }
            auto estimated =
                estimate(features, boundaries, measured->lengths, settings.noteThreshold);
            if (!estimated) {
                return estimated.takeError();
            }

            // 6. Lay the notes out on the host's timeline.
            auto result = layOut(measured->durations, *estimated, audio.startTime, settings.cutoff);

            // A stop that arrived during the last model call may not have reached the session in
            // time. The contract requires a cancelled execution to report Canceled and no result.
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }
            report(1);
            return result;
        }

    private:
        /// The knob values of one execution, in the units the models take.
        struct Settings {
            float boundaryThreshold = 0;
            float noteThreshold = 0;
            double cutoff = 0;

            /// The smallest interval between boundaries, in the model's frames.
            std::int64_t radius = 1;

            int steps = 0;
        };

        /// The encoder output: the features that the other models read, and the frame mask.
        struct Encoded {
            otter::onnx::Tensors features;
            std::vector<std::uint8_t> frames;
        };

        /// The bd2dur output: its tensors, which the estimator reads, and the note durations.
        struct Measured {
            otter::onnx::Tensors lengths;
            std::vector<float> durations;
        };

        /// The estimator output, one entry per note.
        struct Estimated {
            std::vector<float> confidences;
            std::vector<float> keys;
        };

        /// Returns the model's number for the language of an execution.
        ///
        /// \return The model's number for the requested or default language; 0 if neither is
        ///         specified and the model has no language numbering;
        ///         \c srt::Error::InvalidArgument if neither is specified and the model numbers its
        ///         languages, or if the language is not declared or not numbered.
        ///
        /// Zero is not a neutral language number: it is one of the model's own numbers, and the
        /// packages that ship this variant number their languages from one, so zero may not
        /// correspond to any language of the model. A model that numbers languages therefore
        /// requires a language from the execution or from the declared default; an execution
        /// without either is rejected rather than transcribed in the language that zero happens
        /// to denote. A model without language numbering receives zero, its only valid value.
        ///
        /// The language is checked against the exports before the configuration is consulted:
        /// the exports define the languages that the package offers, and the configuration may
        /// number a language that the model supports but the package does not declare.
        srt::Expected<std::int64_t> chooseLanguage(const NoteApi::NoteStartInput &input) const {
            const auto &wanted = input.language.value_or(m_schema.defaultLanguage);
            if (wanted.empty()) {
                if (!m_configuration.languages.empty()) {
                    return srt::Error(srt::Error::InvalidArgument,
                                      "this model numbers its languages, and neither the "
                                      "execution nor the declaration specifies a language");
                }
                return 0;
            }
            const auto &declared = m_schema.languages;
            if (std::find(declared.begin(), declared.end(), wanted) == declared.end()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this package does not declare the language " + wanted);
            }
            const auto it = m_configuration.languages.find(wanted);
            if (it == m_configuration.languages.end()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this model has no number for the language " + wanted);
            }
            return it->second;
        }

        /// Chooses every knob of an execution.
        ///
        /// Every knob is checked against the range that the declaration reports, so a value
        /// outside the declared range is rejected instead of being passed to a model with
        /// unpredictable results. This check matters most for `steps`, because a negative count
        /// produced an empty schedule tensor before the check existed.
        ///
        /// \return The settings, or the error of the first knob outside its declared range.
        srt::Expected<Settings> chooseSettings(const NoteApi::NoteStartInput &input) const {
            double boundaryThreshold = 0;
            double noteThreshold = 0;
            double radiusSeconds = 0;
            Settings settings;
            const std::tuple<const std::optional<double> &, const CommonApi::Knob &, double,
                             double *, const char *>
                knobs[] = {
                    {input.boundaryThreshold,  m_schema.boundaryThreshold,
                     FALLBACK_BOUNDARY_THRESHOLD,                                                     &boundaryThreshold, "boundaryThreshold" },
                    {input.noteThreshold,      m_schema.noteThreshold,      FALLBACK_NOTE_THRESHOLD,
                     &noteThreshold,                                                                                      "noteThreshold"     },
                    {input.notePresenceCutoff, m_schema.notePresenceCutoff,
                     FALLBACK_NOTE_PRESENCE_CUTOFF,                                                   &settings.cutoff,   "notePresenceCutoff"},
                    {input.boundaryRadius,     m_schema.boundaryRadius,     FALLBACK_BOUNDARY_RADIUS,
                     &radiusSeconds,                                                                                      "boundaryRadius"    },
            };
            for (const auto &[given, knob, fallback, target, what] : knobs) {
                auto chosen = otter::chooseKnob(given, knob, fallback, what);
                if (!chosen) {
                    return chosen.takeError();
                }
                *target = chosen.take();
            }
            auto steps = otter::chooseKnob(input.steps, m_schema.steps, FALLBACK_STEPS, "steps");
            if (!steps) {
                return steps.takeError();
            }
            settings.steps = steps.take();
            settings.boundaryThreshold = static_cast<float>(boundaryThreshold);
            settings.noteThreshold = static_cast<float>(noteThreshold);
            // The model counts the radius in its own frames; the contract states it in seconds so
            // that its meaning to a host is independent of the model's frame rate.
            settings.radius = std::max<std::int64_t>(
                1,
                static_cast<std::int64_t>(std::llround(radiusSeconds / m_configuration.timestep)));
            return settings;
        }

        /// Runs the encoder over the span.
        srt::Expected<Encoded> encode(const otter::PreparedSamples &waveform,
                                      double duration) const {
            otter::onnx::Tensors inputs;
            {
                auto tensor = otter::onnx::waveformTensor(waveform);
                if (!tensor) {
                    return tensor.takeError();
                }
                inputs[tensors::WAVEFORM] = tensor.take();
            }
            {
                const std::vector<float> value = {static_cast<float>(duration)};
                auto tensor = floats({1}, value);
                if (!tensor) {
                    return tensor.takeError();
                }
                inputs[tensors::DURATION] = tensor.take();
            }
            auto encoded =
                runModel(*m_models.encoder, std::move(inputs),
                         {tensors::X_SEG, tensors::X_EST, tensors::MASK_T}, "encoder model");
            if (!encoded) {
                return encoded.takeError();
            }
            Encoded result;
            result.features = encoded.take();
            auto maskT = readFlags(result.features.at(tensors::MASK_T), tensors::MASK_T);
            if (!maskT) {
                return maskT.takeError();
            }
            result.frames = maskT.take();
            if (result.frames.empty()) {
                return srt::Error(otter::AnalysisError::ModelFailed,
                                  "the encoder produced no frames for this span");
            }
            return result;
        }

        /// Places the note boundaries by iterating over the sampling schedule.
        ///
        /// The sampling loop runs in the provider, not in the model. Every segmenter input shares
        /// one batch dimension, `t` included, so a call carries exactly one timestep. Passing
        /// the whole schedule would give `t` a batch size equal to the number of steps and every
        /// other input a batch size of one, which the model rejects by shape. The steps are
        /// therefore iterated here, and each step refines the boundaries of the previous step.
        srt::Expected<TensorPtr> segment(const Encoded &encoded,
                                         const std::vector<std::uint8_t> &known,
                                         std::int64_t languageId, const Settings &settings,
                                         const std::function<void(double)> &report) const {
            const auto frameCount = static_cast<std::int64_t>(encoded.frames.size());
            otter::onnx::Tensors fixed;
            fixed[tensors::X_SEG] = encoded.features.at(tensors::X_SEG);
            fixed[tensors::MASK_T] = encoded.features.at(tensors::MASK_T);
            {
                auto tensor = flags({1, frameCount}, known);
                if (!tensor) {
                    return tensor.takeError();
                }
                // The second input holds the output of the previous sampling step. The first step
                // has no previous step, so the input starts as the known boundaries, which are all
                // zero if the host supplies no known notes.
                fixed[tensors::KNOWN_BOUNDARIES] = tensor.take();
                auto again = flags({1, frameCount}, known);
                if (!again) {
                    return again.takeError();
                }
                fixed[tensors::PREV_BOUNDARIES] = again.take();
            }
            {
                const std::vector<std::int64_t> value = {languageId};
                auto tensor = ds::Tensor::createFromView<std::int64_t>(
                    {1}, stdc::array_view<std::int64_t>{value});
                if (!tensor) {
                    return tensor.takeError();
                }
                fixed[tensors::LANGUAGE] = TensorPtr(tensor.take());
            }
            // Both inputs are scalars instead of one-element vectors, because the model declares
            // them without dimensions and rejects a different rank instead of broadcasting it.
            {
                const std::vector<float> value = {settings.boundaryThreshold};
                auto tensor = floats({}, value);
                if (!tensor) {
                    return tensor.takeError();
                }
                fixed[tensors::THRESHOLD] = tensor.take();
            }
            {
                const std::vector<std::int64_t> value = {settings.radius};
                auto tensor = ds::Tensor::createFromView<std::int64_t>(
                    {}, stdc::array_view<std::int64_t>{value});
                if (!tensor) {
                    return tensor.takeError();
                }
                fixed[tensors::RADIUS] = TensorPtr(tensor.take());
            }

            const auto points = schedule(m_configuration.scheduleStart, settings.steps);
            TensorPtr boundaries;
            for (std::size_t step = 0; step < points.size(); ++step) {
                if (auto stopped = checkCancelled(); !stopped) {
                    return stopped.takeError();
                }
                auto inputs = fixed;
                {
                    const std::vector<float> value = {points[step]};
                    auto tensor = floats({1}, value);
                    if (!tensor) {
                        return tensor.takeError();
                    }
                    inputs[tensors::SCHEDULE_TIME] = tensor.take();
                }
                if (boundaries) {
                    inputs[tensors::PREV_BOUNDARIES] = boundaries;
                }
                auto segmented = runModel(*m_models.segmenter, std::move(inputs),
                                          {tensors::BOUNDARIES}, "segmenter model");
                if (!segmented) {
                    return segmented.takeError();
                }
                boundaries = segmented.take().at(tensors::BOUNDARIES);
                report(0.4 +
                       0.2 * static_cast<double>(step + 1) / static_cast<double>(points.size()));
            }
            if (!boundaries) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "the sampling schedule is empty, so nothing was segmented");
            }
            return boundaries;
        }

        /// Converts the boundaries into note durations.
        srt::Expected<Measured> measure(const TensorPtr &boundaries,
                                        const otter::onnx::Tensors &features) const {
            otter::onnx::Tensors inputs;
            inputs[tensors::BOUNDARIES] = boundaries;
            inputs[tensors::MASK_T] = features.at(tensors::MASK_T);
            auto measured = runModel(*m_models.boundaryToDuration, std::move(inputs),
                                     {tensors::DURATIONS, tensors::MASK_N}, "bd2dur model");
            if (!measured) {
                return measured.takeError();
            }
            Measured result;
            result.lengths = measured.take();
            auto durations =
                otter::onnx::readFloats(result.lengths.at(tensors::DURATIONS), tensors::DURATIONS);
            if (!durations) {
                return durations.takeError();
            }
            result.durations = durations.take();
            return result;
        }

        /// Assigns each note a pitch and a confidence.
        srt::Expected<Estimated> estimate(const otter::onnx::Tensors &features,
                                          const TensorPtr &boundaries,
                                          const otter::onnx::Tensors &lengths,
                                          float noteThreshold) const {
            otter::onnx::Tensors inputs;
            inputs[tensors::X_EST] = features.at(tensors::X_EST);
            inputs[tensors::BOUNDARIES] = boundaries;
            inputs[tensors::MASK_T] = features.at(tensors::MASK_T);
            inputs[tensors::MASK_N] = lengths.at(tensors::MASK_N);
            {
                // The model declares this input as a scalar, the same rank as the segmenter's two
                // knobs, and rejects a one-element vector instead of broadcasting it.
                const std::vector<float> value = {noteThreshold};
                auto tensor = floats({}, value);
                if (!tensor) {
                    return tensor.takeError();
                }
                inputs[tensors::THRESHOLD] = tensor.take();
            }
            auto estimated = runModel(*m_models.estimator, std::move(inputs),
                                      {tensors::PRESENCE, tensors::SCORES}, "estimator model");
            if (!estimated) {
                return estimated.takeError();
            }
            const auto pitches = estimated.take();
            auto presence = readConfidences(pitches.at(tensors::PRESENCE), tensors::PRESENCE);
            if (!presence) {
                return presence.takeError();
            }
            auto scores = otter::onnx::readFloats(pitches.at(tensors::SCORES), tensors::SCORES);
            if (!scores) {
                return scores.takeError();
            }
            return Estimated{presence.take(), scores.take()};
        }

        /// Lays the notes out on the host's timeline, dropping those below the cutoff.
        ///
        /// The model produces the durations in seconds, so the placement is a running sum. The
        /// original implementation converted each duration to ticks at a single tempo, which
        /// quantized the durations and misplaced every note in a score with tempo changes.
        static std::unique_ptr<NoteApi::NoteResult> layOut(const std::vector<float> &durations,
                                                           const Estimated &estimated,
                                                           double startTime, double cutoff) {
            auto result = std::make_unique<NoteApi::NoteResult>();
            double at = 0;
            for (std::size_t i = 0; i < durations.size(); ++i) {
                const double length = durations[i];
                const double confidence =
                    i < estimated.confidences.size() ? estimated.confidences[i] : 0.0;
                if (confidence >= cutoff && i < estimated.keys.size()) {
                    result->notes.push_back({static_cast<int>(std::lround(estimated.keys[i])),
                                             startTime + at, length, confidence});
                }
                at += length;
            }
            return result;
        }

        /// Converts the notes known to the caller into the boundary mask that the segmenter takes.
        ///
        /// The notes have passed otter::support::checkKnownNotes(), so they lie inside the span in
        /// ascending order.
        srt::Expected<std::vector<std::uint8_t>>
            knownBoundaries(const std::vector<NoteApi::KnownNote> &notes, double startTime,
                            const std::vector<std::uint8_t> &frames) const {
            std::vector<float> lengths;
            lengths.reserve(notes.size());
            // Known notes are stated on the host's timeline, like all other times in the contract,
            // so they are shifted into the span before they reach the model. Reading them as
            // offsets into the span would silently misplace every boundary whenever a host analyzed
            // any slice other than the first.
            double previousEnd = 0;
            for (const auto &note : notes) {
                const auto begin = note.start - startTime;
                // The model reads a sequence of durations, so a gap between two notes must also be
                // a duration; otherwise every note after the gap would be placed early.
                if (begin > previousEnd + otter::support::TIME_EPSILON) {
                    lengths.push_back(static_cast<float>(begin - previousEnd));
                }
                lengths.push_back(static_cast<float>(note.duration));
                previousEnd = begin + note.duration;
            }

            otter::onnx::Tensors inputs;
            {
                auto tensor = floats({1, static_cast<std::int64_t>(lengths.size())}, lengths);
                if (!tensor) {
                    return tensor.takeError();
                }
                inputs[tensors::DURATIONS] = tensor.take();
            }
            {
                auto tensor = flags({1, static_cast<std::int64_t>(frames.size())}, frames);
                if (!tensor) {
                    return tensor.takeError();
                }
                inputs[tensors::MASK_T] = tensor.take();
            }
            auto converted = runModel(*m_models.durationToBoundary, std::move(inputs),
                                      {tensors::BOUNDARIES}, "dur2bd model");
            if (!converted) {
                return converted.takeError();
            }
            return readFlags(converted.take().at(tensors::BOUNDARIES), tensors::BOUNDARIES);
        }

        /// Owned by OnnxExecutive; the alignment model is null if the package ships no alignment
        /// model.
        Models<ds::InferenceSession *> m_models;

        /// Owned by the spec, which outlives every executive created from it.
        const NoteApi::NoteSchema &m_schema;
        const GameConfiguration &m_configuration;
    };

    class GameInterpreter : public otter::AnalysisInterpreter {
    public:
        GameInterpreter()
            : AnalysisInterpreter(NoteApi::API_INTERFACE, NoteApi::API_LEVEL, VARIANT) {
        }

        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override {
            auto schema = NoteApi::readNoteSchema(spec, VARIANT);
            if (!schema) {
                return schema.takeError();
            }
            auto configuration = readConfiguration(spec);
            if (!configuration) {
                return configuration.takeError();
            }
            const auto &declared = **schema;
            const auto &wiring = **configuration;
            // The library checks the contract syntax; whether this package can honor its
            // declaration is checked here, where both blocks are available. A language that the
            // exports declare but the model cannot number, or an alignment path declared without
            // the model that performs it, would otherwise surface as a failed execution long after
            // the package loaded.
            //
            // The audio format is a declaration of the same kind, and leaving it unchecked is more
            // harmful: the host prepares the audio exactly as declared, so a sample rate at which
            // these models were not trained does not cause a failure. The models then transcribe
            // at the wrong speed, and the result is a plausible transcription an octave away from
            // the correct pitch.
            if (declared.sampleRate != MODEL_SAMPLE_RATE) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the game variant runs at " + std::to_string(MODEL_SAMPLE_RATE) +
                                      " Hz; the exports declare " +
                                      std::to_string(declared.sampleRate));
            }
            if (declared.channelCount != 1) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "the game variant feeds its models one channel, and the exports "
                                  "declare " +
                                      std::to_string(declared.channelCount));
            }
            for (const auto &language : declared.languages) {
                if (wiring.languages.find(language) == wiring.languages.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "the exports list the language " + language +
                                          " but the configuration gives it no numbering");
                }
            }
            // A model that numbers languages has no neutral fallback number, so a package whose
            // exports list no languages leaves an execution without a number to pass. The exports
            // reader has already required a default language among the listed languages.
            if (!wiring.languages.empty() && declared.languages.empty()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the configuration numbers languages but the exports list none, "
                                  "so an execution has no language to pass to the model");
            }
            if (declared.supportsKnownNotes && wiring.durationToBoundary.empty()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the exports declare supportsKnownNotes but the configuration "
                                  "names no durationToBoundary model");
            }
            return std::unique_ptr<srt::ContribExports>(schema.take().release());
        }

        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override {
            auto result = readConfiguration(spec);
            if (!result) {
                return result.takeError();
            }
            return std::unique_ptr<srt::ContribConfiguration>(result.take().release());
        }

        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override {
            const auto configuration =
                spec.configuration() ? spec.configuration()->as<GameConfiguration>() : nullptr;
            const auto schema =
                spec.exports() ? spec.exports()->as<NoteApi::NoteSchema>() : nullptr;
            if (configuration == nullptr || schema == nullptr) {
                return srt::Error(otter::AnalysisError::Internal,
                                  "this declaration carries no game configuration");
            }

            OpenedModels models;
            const std::tuple<const std::filesystem::path *,
                             std::unique_ptr<ds::InferenceSession> OpenedModels::*, const char *>
                wanted[] = {
                    {&configuration->encoder,            &OpenedModels::encoder,            "encoder model"  },
                    {&configuration->segmenter,          &OpenedModels::segmenter,          "segmenter model"},
                    {&configuration->estimator,          &OpenedModels::estimator,          "estimator model"},
                    {&configuration->boundaryToDuration, &OpenedModels::boundaryToDuration,
                     "bd2dur model"                                                                          },
                    {&configuration->durationToBoundary, &OpenedModels::durationToBoundary,
                     "dur2bd model"                                                                          },
            };
            for (const auto &[path, member, what] : wanted) {
                if (path->empty()) {
                    // Only the alignment model may be absent; the reader has already rejected a
                    // declaration that lacks any of the other models.
                    continue;
                }
                auto session = otter::onnx::openSession(spec, *path, what);
                if (!session) {
                    return session.takeError();
                }
                models.*member = session.take();
            }
            return std::unique_ptr<srt::InferenceExecutive>(
                new GameExecutive(spec, std::move(models), *schema, *configuration));
        }

    private:
        static srt::Expected<std::unique_ptr<GameConfiguration>>
            readConfiguration(const srt::ContribSpec &spec) {
            const auto &value = spec.manifestConfiguration();
            if (!value.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the game configuration must be an object");
            }
            const auto object = value.toObject();
            if (auto checked = otter::manifest::rejectUnknownKeys(
                    object,
                    {"encoder", "segmenter", "estimator", "boundaryToDuration",
                     "durationToBoundary", "timestep", "languages", "scheduleStart"},
                    "the game configuration");
                !checked) {
                return checked.takeError();
            }

            auto result = std::make_unique<GameConfiguration>();

            if (auto read = otter::onnx::readPaths(
                    object, spec.declarationPath().parent_path(),
                    {
                        {"encoder",            &result->encoder           },
                        {"segmenter",          &result->segmenter         },
                        {"estimator",          &result->estimator         },
                        {"boundaryToDuration", &result->boundaryToDuration},
                        {"durationToBoundary", &result->durationToBoundary}
            },
                    {"encoder", "segmenter", "estimator", "boundaryToDuration"},
                    "the game configuration");
                !read) {
                return read.takeError();
            }

            if (const auto it = object.find("timestep"); it != object.end()) {
                auto number = otter::manifest::readPositiveDouble(it->second, "timestep");
                if (!number) {
                    return number.takeError();
                }
                result->timestep = number.take();
            }
            if (const auto it = object.find("scheduleStart"); it != object.end()) {
                auto number = otter::manifest::readUnitDouble(it->second, "scheduleStart");
                if (!number) {
                    return number.takeError();
                }
                result->scheduleStart = number.take();
            }

            if (const auto it = object.find("languages"); it != object.end()) {
                if (!it->second.isObject()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "languages must map identifiers to the model's numbering");
                }
                for (const auto &[name, id] : it->second.toObject()) {
                    if (name.empty()) {
                        return srt::Error(srt::Error::InvalidFormat,
                                          "a language identifier must not be empty");
                    }
                    if (!id.isInt() || id.toInt() < 0) {
                        return srt::Error(srt::Error::InvalidFormat,
                                          "the numbering of " + name +
                                              " must be a non-negative integer");
                    }
                    result->languages.emplace(name, static_cast<int>(id.toInt()));
                }
            }
            return result;
        }
    };

    class GamePlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(std::string_view interfaceName, int level, std::string_view variant) override {
            if (auto served =
                    otter::onnx::checkServed(interfaceName, level, variant, NoteApi::API_INTERFACE,
                                             NoteApi::API_LEVEL, VARIANT);
                !served) {
                return served.takeError();
            }
            return std::unique_ptr<srt::ContribInterpreter>(new GameInterpreter());
        }
    };

}

STDC_EXPORT_PLUGIN(GamePlugin)
