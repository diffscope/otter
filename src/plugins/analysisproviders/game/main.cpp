// The note transcription provider, ported from the GAME extractor.
//
// Four models in sequence: an encoder turns audio into features, a segmenter places note
// boundaries, one model turns boundaries into durations, and an estimator gives each note a pitch
// and a confidence. A fifth, optional, turns durations the caller already knows back into
// boundaries, which is how transcription is conditioned on an existing score.
//
// Two things differ from the implementation this came from. Time is seconds throughout rather than
// ticks at a fixed tempo, so nothing here needs to know a tempo and a score whose tempo moves is
// no longer placed wrongly. And the fifth model is wired up: the weights always had it, the port
// it came from dropped the session, and the segmenter has been receiving zeros for known
// boundaries ever since.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <stdcorelib/path.h>
#include <stdcorelib/plugin/plugin.h>

#include <synthrt/Core/PackageHandle.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/Support/Expected.h>
#include <synthrt/Support/JSON.h>

#include <dsinfer/Api/Drivers/Onnx/OnnxDriverApi.h>
#include <dsinfer/Core/Tensor.h>
#include <dsinfer/Inference/InferenceDriver.h>
#include <dsinfer/Inference/InferenceDriverPlugin.h>
#include <dsinfer/Inference/InferenceSession.h>

#include <otter/Analysis/AnalysisInput.h>
#include <otter/Analysis/AnalysisProvider.h>
#include <otter/Analysis/AnalysisProviderPlugin.h>
#include <otter/Analysis/AnalysisRunner.h>
#include <otter/Api/Note/1/NoteApiL1.h>
#include <otter/Support/ManifestValues.h>

namespace NoteApi = otter::Api::Note::L1;
namespace CommonApi = otter::Api::Common::L1;
namespace OnnxApi = ds::Api::Onnx;

namespace {

    constexpr char VARIANT[] = "game";
    constexpr char BACKEND[] = "onnx";

    /// Knob values used when the declaration honors no such knob.
    constexpr int FALLBACK_STEPS = 8;
    constexpr double FALLBACK_BOUNDARY_THRESHOLD = 0.2;
    constexpr double FALLBACK_BOUNDARY_RADIUS = 0.02;
    constexpr double FALLBACK_NOTE_THRESHOLD = 0.2;
    constexpr double FALLBACK_NOTE_PRESENCE_CUTOFF = 0.5;

    /// What this variant reads from its configuration block: where its models are and how they
    /// are wired, which is what the host never needs to know. The audio format, the language
    /// identifiers and the knobs are contract facts and live in exports.
    class GameConfiguration : public srt::ContribConfiguration {
    public:
        GameConfiguration()
            : srt::ContribConfiguration(NoteApi::API_INTERFACE, VARIANT, NoteApi::API_LEVEL) {
        }

        std::filesystem::path encoder;
        std::filesystem::path segmenter;
        std::filesystem::path estimator;
        std::filesystem::path boundaryToDuration;

        /// Empty when this package ships no alignment model.
        std::filesystem::path durationToBoundary;

        /// Model frame rate in seconds. Used only to convert boundaryRadius into frames.
        double timestep = 0.01;

        /// Maps the language identifiers the exports list to the model's own numbering.
        ///
        /// The contract speaks identifiers because a model's internal numbering is its own: two
        /// models need not agree that 1 is the same language.
        std::map<std::string, int> languages;

        /// Language used when the caller supplies none.
        std::string defaultLanguage;

        /// Start of the sampling schedule.
        double scheduleStart = 0;
    };

    using TensorPtr = std::shared_ptr<ds::ITensor>;

    /// The sampling schedule: \a steps evenly spaced points from \a start up to one.
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
                                    const std::vector<float> &values) {
        auto tensor = ds::Tensor::createFromView<float>(shape, stdc::array_view<float>{values});
        if (!tensor) {
            return tensor.takeError();
        }
        return TensorPtr(tensor.take());
    }

    srt::Expected<TensorPtr> flags(const std::vector<std::int64_t> &shape,
                                   const std::vector<std::uint8_t> &values) {
        // Bool tensors are one byte an element, which is what the raw view expects.
        const stdc::array_view<std::byte> raw(reinterpret_cast<const std::byte *>(values.data()),
                                              values.size());
        auto tensor = ds::Tensor::createFromRawView(ds::ITensor::Bool, shape, raw);
        if (!tensor) {
            return tensor.takeError();
        }
        return TensorPtr(tensor.take());
    }

    /// Reads a bool tensor back as one byte an element.
    srt::Expected<std::vector<std::uint8_t>> readFlags(const TensorPtr &tensor, const char *what) {
        if (!tensor || tensor->dataType() != ds::ITensor::Bool) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " is not a boolean tensor");
        }
        const auto raw = tensor->rawView();
        std::vector<std::uint8_t> result(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            result[i] = raw[i] == std::byte{0} ? 0 : 1;
        }
        return result;
    }

    srt::Expected<std::vector<float>> readFloats(const TensorPtr &tensor, const char *what) {
        if (!tensor || tensor->dataType() != ds::ITensor::Float) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " is not a float tensor");
        }
        const auto view = tensor->view<float>();
        return std::vector<float>(view.begin(), view.end());
    }

    /// Reads a confidence that an export may have written as either a number or a flag.
    ///
    /// The shipped GAME model says whether a note is there with a boolean; the contract reads a
    /// confidence between zero and one, and a flag is a confidence with two values. Which one
    /// arrives is not guessed: the tensor carries its own type, so both are read and nothing has
    /// to be declared about it.
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
        return readFloats(tensor, what);
    }

    /// Runs one session and hands back the outputs it was asked for.
    srt::Expected<std::map<std::string, TensorPtr>> invoke(ds::InferenceSession &session,
                                                           std::map<std::string, TensorPtr> inputs,
                                                           const std::set<std::string> &wanted,
                                                           const char *what) {
        OnnxApi::SessionStartInput request;
        request.inputs = std::move(inputs);
        request.outputs = wanted;

        auto response = session.start(request);
        if (!response) {
            return response.takeError().withContext(std::string("the ") + what + " model failed");
        }
        auto produced = response.take();
        auto outputs = produced ? produced->as<OnnxApi::SessionResult>() : nullptr;
        if (outputs == nullptr) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string("the ") + what + " model returned no outputs");
        }
        for (const auto &name : wanted) {
            const auto it = outputs->outputs.find(name);
            if (it == outputs->outputs.end() || !it->second) {
                return srt::Error(srt::Error::InvalidFormat,
                                  std::string("the ") + what + " model did not return " + name);
            }
        }
        return outputs->outputs;
    }

    /// The sessions one analyzer owns, opened together and closed together.
    struct Models {
        std::unique_ptr<ds::InferenceSession> encoder;
        std::unique_ptr<ds::InferenceSession> segmenter;
        std::unique_ptr<ds::InferenceSession> estimator;
        std::unique_ptr<ds::InferenceSession> boundaryToDuration;
        std::unique_ptr<ds::InferenceSession> durationToBoundary;

        void forEach(const std::function<void(ds::InferenceSession &)> &body) {
            for (auto session :
                 {&encoder, &segmenter, &estimator, &boundaryToDuration, &durationToBoundary}) {
                if (*session) {
                    body(**session);
                }
            }
        }
    };

    class GameExecutive : public NoteApi::NoteExecutive {
    public:
        GameExecutive(otter::AnalysisSpec &spec, Models models, const NoteApi::NoteSchema &schema,
                      const GameConfiguration &configuration)
            : NoteExecutive(spec), m_models(std::move(models)), m_sampleRate(schema.sampleRate),
              m_channelCount(schema.channelCount), m_maxSegmentDuration(schema.maxSegmentDuration),
              m_supportsKnownNotes(schema.supportsKnownNotes),
              m_boundaryThreshold(schema.boundaryThreshold),
              m_boundaryRadius(schema.boundaryRadius), m_noteThreshold(schema.noteThreshold),
              m_notePresenceCutoff(schema.notePresenceCutoff), m_steps(schema.steps),
              m_timestep(configuration.timestep), m_languages(configuration.languages),
              m_defaultLanguage(configuration.defaultLanguage),
              m_scheduleStart(configuration.scheduleStart) {
        }

        ~GameExecutive() {
            m_runner.cancel();
            m_runner.wait();
            m_models.forEach([](ds::InferenceSession &session) {
                session.stop();
                session.close();
            });
        }

        srt::ITask::State state() const noexcept override {
            return m_runner.state();
        }

        srt::Expected<void> stop() override {
            m_runner.cancel();
            m_models.forEach([](ds::InferenceSession &session) { session.stop(); });
            return srt::Expected<void>();
        }

        srt::Expected<void> waitForFinished() override {
            m_runner.wait();
            // Every session is waited on even after one reports a failure, because leaving a
            // session running is what this call exists to prevent. The first failure is the one
            // reported.
            std::optional<srt::Error> failure;
            m_models.forEach([&failure](ds::InferenceSession &session) {
                if (auto waited = session.waitForFinished(); !waited && !failure) {
                    failure = waited.error();
                }
            });
            if (failure) {
                return *failure;
            }
            return srt::Expected<void>();
        }

        srt::Expected<std::unique_ptr<NoteApi::NoteResult>>
            start(const NoteApi::NoteStartInput &input) override {
            if (!m_runner.begin()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this analyzer is already running an execution");
            }
            auto result = run(input);
            m_runner.end(static_cast<bool>(result));
            return result;
        }

        srt::Expected<void> startAsync(std::shared_ptr<const NoteApi::NoteStartInput> input,
                                       AsyncCallback callback) override {
            if (!input) {
                return srt::Error(srt::Error::InvalidArgument, "no input was supplied");
            }
            if (!m_runner.begin()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this analyzer is already running an execution");
            }
            // A failure to spawn releases the claim itself; there is no body left to do it.
            return m_runner.spawn([this, input, callback = std::move(callback)]() mutable {
                auto result = run(*input);
                m_runner.end(static_cast<bool>(result));
                if (callback) {
                    callback(std::move(result));
                }
            });
        }

    private:
        srt::Expected<void> cancelled() const {
            if (m_runner.cancelled()) {
                return srt::Error(srt::Error::InvalidArgument, "the execution was cancelled");
            }
            return srt::Expected<void>();
        }

        srt::Expected<std::unique_ptr<NoteApi::NoteResult>>
            run(const NoteApi::NoteStartInput &input) {
            const auto &audio = input.audio;
            auto prepared =
                otter::prepareSamples(audio, m_sampleRate, m_channelCount, m_maxSegmentDuration);
            if (!prepared) {
                return prepared.takeError();
            }
            const auto waveform = prepared.take();
            const auto duration = audio.duration();

            std::int64_t languageId = 0;
            {
                const auto &wanted = input.language.value_or(m_defaultLanguage);
                if (!wanted.empty()) {
                    const auto it = m_languages.find(wanted);
                    if (it == m_languages.end()) {
                        return srt::Error(srt::Error::InvalidArgument,
                                          "this model does not know the language " + wanted);
                    }
                    languageId = it->second;
                }
            }

            // Every knob is checked against the range the declaration reports, so a value the
            // module said it would not take is refused rather than handed to a model to do
            // something unpredictable with. `steps` is the one that used to matter most: a
            // negative count produced an empty schedule tensor.
            double boundaryThresholdValue = 0;
            double noteThresholdValue = 0;
            double cutoff = 0;
            double radiusSeconds = 0;
            int steps = 0;
            const std::tuple<const std::optional<double> &, const CommonApi::Knob &, double,
                             double *, const char *>
                knobs[] = {
                    {input.boundaryThreshold,  m_boundaryThreshold,  FALLBACK_BOUNDARY_THRESHOLD,
                     &boundaryThresholdValue, "boundaryThreshold" },
                    {input.noteThreshold,      m_noteThreshold,      FALLBACK_NOTE_THRESHOLD,
                     &noteThresholdValue,     "noteThreshold"     },
                    {input.notePresenceCutoff, m_notePresenceCutoff, FALLBACK_NOTE_PRESENCE_CUTOFF,
                     &cutoff,                 "notePresenceCutoff"},
                    {input.boundaryRadius,     m_boundaryRadius,     FALLBACK_BOUNDARY_RADIUS,
                     &radiusSeconds,          "boundaryRadius"    },
            };
            for (const auto &[given, knob, fallback, target, what] : knobs) {
                auto chosen = otter::chooseKnob(given, knob, fallback, what);
                if (!chosen) {
                    return chosen.takeError();
                }
                *target = chosen.take();
            }
            {
                auto chosen = otter::chooseKnob(input.steps, m_steps, FALLBACK_STEPS, "steps");
                if (!chosen) {
                    return chosen.takeError();
                }
                steps = chosen.take();
            }
            const auto boundaryThreshold = static_cast<float>(boundaryThresholdValue);
            const auto noteThreshold = static_cast<float>(noteThresholdValue);
            // The model counts the radius in its own frames; the contract states it in seconds so
            // that it means the same thing to a host whatever frame rate the model runs at.
            const auto radius = std::max<std::int64_t>(
                1, static_cast<std::int64_t>(std::llround(radiusSeconds / m_timestep)));

            const auto report = [&input](double value) {
                if (input.progress) {
                    input.progress(value);
                }
            };
            report(0);

            // 1. Encode.
            if (auto stopped = cancelled(); !stopped) {
                return stopped.takeError();
            }
            std::map<std::string, TensorPtr> encoderInputs;
            {
                auto tensor = floats({1, static_cast<std::int64_t>(waveform.size())}, waveform);
                if (!tensor) {
                    return tensor.takeError();
                }
                encoderInputs["waveform"] = tensor.take();
            }
            const std::vector<float> durationValue = {static_cast<float>(duration)};
            {
                auto tensor = floats({1}, durationValue);
                if (!tensor) {
                    return tensor.takeError();
                }
                encoderInputs["duration"] = tensor.take();
            }
            auto encoded = invoke(*m_models.encoder, std::move(encoderInputs),
                                  {"x_seg", "x_est", "maskT"}, "encoder");
            if (!encoded) {
                return encoded.takeError();
            }
            const auto features = encoded.take();
            auto maskT = readFlags(features.at("maskT"), "maskT");
            if (!maskT) {
                return maskT.takeError();
            }
            const auto frames = maskT.take();
            if (frames.empty()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the encoder produced no frames for this span");
            }
            const auto frameCount = static_cast<std::int64_t>(frames.size());
            report(0.3);

            // 2. Known boundaries, when the caller supplied a score to align against.
            std::vector<std::uint8_t> known(frames.size(), 0);
            if (!input.knownNotes.empty()) {
                if (!m_supportsKnownNotes || !m_models.durationToBoundary) {
                    return srt::Error(srt::Error::FeatureNotSupported,
                                      "this model cannot be conditioned on known notes");
                }
                auto converted = knownBoundaries(input.knownNotes, audio.startTime, frames);
                if (!converted) {
                    return converted.takeError();
                }
                known = converted.take();
            }

            // 3. Segment.
            if (auto stopped = cancelled(); !stopped) {
                return stopped.takeError();
            }
            std::map<std::string, TensorPtr> segmenterInputs;
            segmenterInputs["x_seg"] = features.at("x_seg");
            segmenterInputs["maskT"] = features.at("maskT");
            {
                auto tensor = flags({1, frameCount}, known);
                if (!tensor) {
                    return tensor.takeError();
                }
                // The second slot is what the previous sampling step produced, and on the first
                // step there is none -- so it starts as what the caller knows, which is also what
                // a host that does not supply known notes leaves empty.
                segmenterInputs["known_boundaries"] = tensor.take();
                auto again = flags({1, frameCount}, known);
                if (!again) {
                    return again.takeError();
                }
                segmenterInputs["prev_boundaries"] = again.take();
            }
            {
                const std::vector<std::int64_t> value = {languageId};
                auto tensor = ds::Tensor::createFromView<std::int64_t>(
                    {1}, stdc::array_view<std::int64_t>{value});
                if (!tensor) {
                    return tensor.takeError();
                }
                segmenterInputs["language"] = TensorPtr(tensor.take());
            }
            // Both are scalars rather than one element vectors: the model declares them with no
            // dimensions at all, and a rank that disagrees is refused rather than broadcast.
            {
                const std::vector<float> value = {boundaryThreshold};
                auto tensor = floats({}, value);
                if (!tensor) {
                    return tensor.takeError();
                }
                segmenterInputs["threshold"] = tensor.take();
            }
            {
                const std::vector<std::int64_t> value = {radius};
                auto tensor = ds::Tensor::createFromView<std::int64_t>(
                    {}, stdc::array_view<std::int64_t>{value});
                if (!tensor) {
                    return tensor.takeError();
                }
                segmenterInputs["radius"] = TensorPtr(tensor.take());
            }

            // The sampling loop is the host's, not the model's.
            //
            // Every segmenter input shares one batch dimension, `t` included, so a call carries
            // exactly one timestep. Handing it the whole schedule makes the batch look like the
            // number of steps to that one input and one to every other, which the model rejects
            // by shape. So the steps are walked here, each refining what the last produced.
            const auto steps_ = schedule(m_scheduleStart, steps);
            TensorPtr boundaries;
            for (std::size_t step = 0; step < steps_.size(); ++step) {
                if (auto stopped = cancelled(); !stopped) {
                    return stopped.takeError();
                }
                auto inputs = segmenterInputs;
                {
                    const std::vector<float> value = {steps_[step]};
                    auto tensor = floats({1}, value);
                    if (!tensor) {
                        return tensor.takeError();
                    }
                    inputs["t"] = tensor.take();
                }
                if (boundaries) {
                    inputs["prev_boundaries"] = boundaries;
                }
                auto segmented =
                    invoke(*m_models.segmenter, std::move(inputs), {"boundaries"}, "segmenter");
                if (!segmented) {
                    return segmented.takeError();
                }
                boundaries = segmented.take().at("boundaries");
                report(0.4 +
                       0.2 * static_cast<double>(step + 1) / static_cast<double>(steps_.size()));
            }
            if (!boundaries) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "the sampling schedule is empty, so nothing was segmented");
            }
            report(0.6);

            // 4. Boundaries to durations.
            if (auto stopped = cancelled(); !stopped) {
                return stopped.takeError();
            }
            auto measured = invoke(*m_models.boundaryToDuration,
                                   {
                                       {"boundaries", boundaries          },
                                       {"maskT",      features.at("maskT")}
            },
                                   {"durations", "maskN"}, "bd2dur");
            if (!measured) {
                return measured.takeError();
            }
            const auto lengths = measured.take();
            auto durations = readFloats(lengths.at("durations"), "durations");
            if (!durations) {
                return durations.takeError();
            }
            const auto noteDurations = durations.take();
            report(0.8);

            // 5. Estimate pitch.
            if (auto stopped = cancelled(); !stopped) {
                return stopped.takeError();
            }
            std::map<std::string, TensorPtr> estimatorInputs = {
                {"x_est",      features.at("x_est")},
                {"boundaries", boundaries          },
                {"maskT",      features.at("maskT")},
                {"maskN",      lengths.at("maskN") },
            };
            {
                // A scalar, as the model declares it -- the same rank the segmenter's two knobs
                // take, and refused rather than broadcast when it is a one element vector.
                const std::vector<float> value = {noteThreshold};
                auto tensor = floats({}, value);
                if (!tensor) {
                    return tensor.takeError();
                }
                estimatorInputs["threshold"] = tensor.take();
            }
            auto estimated = invoke(*m_models.estimator, std::move(estimatorInputs),
                                    {"presence", "scores"}, "estimator");
            if (!estimated) {
                return estimated.takeError();
            }
            const auto pitches = estimated.take();
            auto presence = readConfidences(pitches.at("presence"), "presence");
            if (!presence) {
                return presence.takeError();
            }
            auto scores = readFloats(pitches.at("scores"), "scores");
            if (!scores) {
                return scores.takeError();
            }
            const auto confidences = presence.take();
            const auto keys = scores.take();

            // 6. Lay the notes out on the host's timeline.
            //
            // The durations are seconds as the model produced them, so this is a running sum and
            // nothing else. The implementation this came from converted each one to ticks at a
            // single tempo, which both quantized here and placed every note wrongly in a score
            // whose tempo moves.
            auto result = std::make_unique<NoteApi::NoteResult>();
            double at = 0;
            for (std::size_t i = 0; i < noteDurations.size(); ++i) {
                const double length = noteDurations[i];
                const double confidence = i < confidences.size() ? confidences[i] : 0.0;
                if (confidence >= cutoff && i < keys.size()) {
                    result->notes.push_back({static_cast<int>(std::lround(keys[i])),
                                             audio.startTime + at, length, confidence});
                }
                at += length;
            }
            report(1);
            return result;
        }

        /// Turns the notes the caller already knows into the boundary mask the segmenter takes.
        srt::Expected<std::vector<std::uint8_t>>
            knownBoundaries(const std::vector<NoteApi::KnownNote> &notes, double startTime,
                            const std::vector<std::uint8_t> &frames) {
            std::vector<float> lengths;
            lengths.reserve(notes.size());
            // Known notes are stated on the host's timeline, like everything else the contract
            // carries, so they are moved into the span before the model sees them. Reading them as
            // offsets into the span instead would place every boundary wrongly the moment a host
            // analyzed anything but the first slice, and nothing would say so.
            double previousEnd = 0;
            for (const auto &note : notes) {
                const auto begin = note.start - startTime;
                if (note.duration <= 0) {
                    return srt::Error(srt::Error::InvalidArgument, "a known note has no duration");
                }
                if (begin < -1e-9) {
                    return srt::Error(srt::Error::InvalidArgument,
                                      "a known note starts before the audio does");
                }
                if (begin < previousEnd - 1e-9) {
                    return srt::Error(srt::Error::InvalidArgument,
                                      "the known notes overlap or are out of order");
                }
                // The model reads a run of durations, so a gap between two notes has to be one
                // too, or every note after the gap would be placed early.
                if (begin > previousEnd + 1e-9) {
                    lengths.push_back(static_cast<float>(begin - previousEnd));
                }
                lengths.push_back(static_cast<float>(note.duration));
                previousEnd = begin + note.duration;
            }

            std::map<std::string, TensorPtr> inputs;
            {
                auto tensor = floats({1, static_cast<std::int64_t>(lengths.size())}, lengths);
                if (!tensor) {
                    return tensor.takeError();
                }
                inputs["durations"] = tensor.take();
            }
            {
                auto tensor = flags({1, static_cast<std::int64_t>(frames.size())}, frames);
                if (!tensor) {
                    return tensor.takeError();
                }
                inputs["maskT"] = tensor.take();
            }
            auto converted =
                invoke(*m_models.durationToBoundary, std::move(inputs), {"boundaries"}, "dur2bd");
            if (!converted) {
                return converted.takeError();
            }
            return readFlags(converted.take().at("boundaries"), "boundaries");
        }

        Models m_models;
        int m_sampleRate;
        int m_channelCount;
        double m_maxSegmentDuration;
        bool m_supportsKnownNotes;
        CommonApi::Knob m_boundaryThreshold;
        CommonApi::Knob m_boundaryRadius;
        CommonApi::Knob m_noteThreshold;
        CommonApi::Knob m_notePresenceCutoff;
        CommonApi::IntKnob m_steps;
        double m_timestep;
        std::map<std::string, int> m_languages;
        std::string m_defaultLanguage;
        double m_scheduleStart;
        otter::AnalysisRunner m_runner;
    };

    class GameExtension : public otter::AnalysisExtension {
    public:
        explicit GameExtension(otter::AnalysisSpec &spec)
            : AnalysisExtension(spec, srt::ContribSpecExtensionTraits<otter::AnalysisSpec,
                                                                      NoteApi::NoteExecutive>::ID) {
        }

        srt::Expected<std::unique_ptr<otter::AnalysisExecutive>>
            createAnalyzer(const otter::AnalysisRuntimeOptions &runtimeOptions) override {
            const auto configuration =
                spec().configuration() ? spec().configuration()->as<GameConfiguration>() : nullptr;
            const auto schema =
                spec().exports() ? spec().exports()->as<NoteApi::NoteSchema>() : nullptr;
            if (configuration == nullptr || schema == nullptr) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "this declaration carries no game configuration");
            }

            auto service = spec().package().synthUnit().runtimeService(
                ds::InferenceDriverPlugin::IID, BACKEND);
            auto driver = service ? service->as<ds::InferenceDriver>() : nullptr;
            if (driver == nullptr) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "the onnx inference driver is not registered on this unit");
            }

            Models models;
            const std::pair<const std::filesystem::path *,
                            std::unique_ptr<ds::InferenceSession> Models::*>
                wanted[] = {
                    {&configuration->encoder,            &Models::encoder           },
                    {&configuration->segmenter,          &Models::segmenter         },
                    {&configuration->estimator,          &Models::estimator         },
                    {&configuration->boundaryToDuration, &Models::boundaryToDuration},
                    {&configuration->durationToBoundary, &Models::durationToBoundary},
            };
            for (const auto &[path, member] : wanted) {
                if (path->empty()) {
                    // Only the alignment model is allowed to be absent; the reader has already
                    // refused a declaration missing any of the others.
                    continue;
                }
                auto session = driver->createSession();
                if (!session) {
                    return srt::Error(srt::Error::FeatureNotSupported,
                                      "the driver created no session");
                }
                OnnxApi::SessionOpenArgs args;
                if (auto opened = session->open(*path, args); !opened) {
                    return opened.takeError().withContext("cannot open " +
                                                          stdc::path::to_utf8(*path));
                }
                models.*member = std::move(session);
            }
            return std::unique_ptr<otter::AnalysisExecutive>(
                new GameExecutive(spec(), std::move(models), *schema, *configuration));
        }
    };

    class GameProvider : public otter::AnalysisProvider {
    public:
        GameProvider() : AnalysisProvider(NoteApi::API_INTERFACE, NoteApi::API_LEVEL, VARIANT) {
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
            // The contract syntax is the library's; whether this package can honor what it
            // declares is checked here, where both blocks are in hand. A language the exports
            // promise but the model cannot number, or an alignment path promised without the
            // model that performs it, would otherwise surface as a failed execution long after
            // the package loaded.
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

    protected:
        srt::Expected<std::unique_ptr<otter::AnalysisExtension>>
            createAnalysisExtension(otter::AnalysisSpec &spec) const override {
            return std::unique_ptr<otter::AnalysisExtension>(new GameExtension(spec));
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
                     "durationToBoundary", "timestep", "languages", "defaultLanguage",
                     "scheduleStart"},
                    "the game configuration");
                !checked) {
                return checked.takeError();
            }

            auto result = std::make_unique<GameConfiguration>();

            const auto directory = spec.declarationPath().parent_path();
            const std::pair<const char *, std::filesystem::path GameConfiguration::*> models[] = {
                {"encoder",            &GameConfiguration::encoder           },
                {"segmenter",          &GameConfiguration::segmenter         },
                {"estimator",          &GameConfiguration::estimator         },
                {"boundaryToDuration", &GameConfiguration::boundaryToDuration},
                {"durationToBoundary", &GameConfiguration::durationToBoundary},
            };
            for (const auto &[key, member] : models) {
                const auto it = object.find(key);
                if (it == object.end()) {
                    continue;
                }
                auto path = otter::manifest::readPath(it->second, directory, key);
                if (!path) {
                    return path.takeError();
                }
                result.get()->*member = path.take();
            }
            for (const char *required :
                 {"encoder", "segmenter", "estimator", "boundaryToDuration"}) {
                if (object.find(required) == object.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      std::string("the game configuration needs a ") + required);
                }
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
            if (const auto it = object.find("defaultLanguage"); it != object.end()) {
                auto name = otter::manifest::readString(it->second, "defaultLanguage");
                if (!name) {
                    return name.takeError();
                }
                result->defaultLanguage = name.take();
            }
            if (!result->defaultLanguage.empty() &&
                result->languages.find(result->defaultLanguage) == result->languages.end()) {
                return srt::Error(srt::Error::InvalidFormat, "the default language " +
                                                                 result->defaultLanguage +
                                                                 " is not one this model declares");
            }
            return result;
        }
    };

    class GameProviderPlugin : public otter::AnalysisProviderPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(std::string_view interfaceName, int level, std::string_view variant) override {
            if (interfaceName != NoteApi::API_INTERFACE || level != NoteApi::API_LEVEL ||
                variant != VARIANT) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this plugin serves only " + std::string(NoteApi::API_INTERFACE) +
                                      " level 1 variant " + VARIANT);
            }
            return std::unique_ptr<srt::ContribInterpreter>(new GameProvider());
        }
    };

}

STDC_EXPORT_PLUGIN(GameProviderPlugin)
