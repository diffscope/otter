// The note transcription provider backed by the game.cpp ggml engine, driven as an external
// process.
//
// The ONNX variant of this contract runs five graphs in sessions of its own. Here the package
// names a command line tool instead, and the provider launches it once, keeps it for the lifetime
// of the analyzer and exchanges messages with it: the engine's `serve` mode reads a binary request
// frame on its standard input and answers one JSON line on its standard output. The model file and
// the engine stay outside the host process, so a stop request is honored by terminating the
// process, and a package that carries the engine's own release can be dropped in without a build.
//
// This port differs from the ONNX variant in two respects that a host can observe. The engine call
// is monolithic: progress reports 0 and then 1, and the call itself is not interruptible, but a
// stop does kill the engine process, so a cancelled execution is bounded by the lifetime of one
// request. And the model carries no alignment path, because the serve protocol opens no session for
// the fifth model, so the exports cannot promise known notes.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
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

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/Provider/AnalysisInput.h>
#include <otter/Analysis/Provider/AnalysisInterpreter.h>
#include <otter/Api/Note/1/NoteApiL1.h>
#include <otter/Support/ManifestValues.h>

#include "../external/CliProcess.h"

namespace NoteApi = otter::Api::Note::L1;
namespace CommonApi = otter::Api::Common::L1;

namespace {

    constexpr char VARIANT[] = "game-ggml";

    /// Knob values used if the declaration does not honor the corresponding knob. They are the
    /// engine's own defaults, stated in the contract's units: the radius counts frames inside the
    /// engine and seconds in the contract, so its default is two frames at the model's frame rate.
    constexpr int FALLBACK_STEPS = 1;
    constexpr double FALLBACK_BOUNDARY_THRESHOLD = 0.2;
    constexpr double FALLBACK_BOUNDARY_RADIUS = 0.02;
    constexpr double FALLBACK_NOTE_THRESHOLD = 0.2;
    constexpr double FALLBACK_NOTE_PRESENCE_CUTOFF = 0.5;

    /// The seed of the engine's internal sampling randomness, fixed so that repeated
    /// transcriptions of one span produce one transcription. The engine treats zero as a request
    /// for a seed drawn from random_device, which would make every execution differ.
    constexpr std::uint64_t FIXED_SEED = 1;

    /// The request frame of the engine's `serve` protocol, as its command line documents it: one
    /// magic, the knobs of one execution and the sample count, followed by the float32 waveform.
    constexpr std::uint32_t SERVE_MAGIC_INFERENCE = 0x53455256u; // "VRES"
    constexpr std::uint32_t SERVE_MAGIC_QUIT = 0x54495155u;      // "UQIT"
    constexpr std::size_t SERVE_HEADER_SIZE = 36;

    /// The configuration block of this variant: the engine's command line tool, the model it runs
    /// and the wiring that is internal to the variant and not visible to the host. The audio
    /// format and the language identifiers are contract facts and are declared in exports.
    class GameGgmlConfiguration : public srt::ContribConfiguration {
    public:
        GameGgmlConfiguration()
            : srt::ContribConfiguration(NoteApi::API_INTERFACE, VARIANT, NoteApi::API_LEVEL) {
        }

        /// The engine's command line tool, as shipped in its release.
        std::filesystem::path cli;

        /// The GGUF model the tool runs.
        std::filesystem::path model;

        /// Maps the language identifiers listed in the exports to the model's own numbering.
        ///
        /// The contract uses identifiers because the internal numbering is specific to each model:
        /// the number 1 need not denote the same language in two models.
        std::map<std::string, int> languages;

        /// The model's frame duration in seconds. Used only to convert boundaryRadius into frames.
        double timestep = 0.01;
    };

    /// The knob values of one execution, in the units the engine takes.
    struct Settings {
        float boundaryThreshold = 0;
        float noteThreshold = 0;
        double cutoff = 0;

        /// The smallest interval between boundaries, in the model's frames.
        int radius = 1;

        int steps = 0;
    };

    /// Chooses every knob of an execution.
    ///
    /// Every knob is checked against the range that the declaration reports, so a value outside
    /// the declared range is rejected instead of being passed to the engine with unpredictable
    /// results.
    ///
    /// \return The settings, or the error of the first knob outside its declared range.
    srt::Expected<Settings> chooseSettings(const NoteApi::NoteStartInput &input,
                                           const NoteApi::NoteSchema &schema,
                                           const GameGgmlConfiguration &configuration) {
        double boundaryThreshold = 0;
        double noteThreshold = 0;
        double radiusSeconds = 0;
        Settings settings;
        const std::tuple<const std::optional<double> &, const CommonApi::Knob &, double, double *,
                         const char *>
            knobs[] = {
                {input.boundaryThreshold,  schema.boundaryThreshold,
                 FALLBACK_BOUNDARY_THRESHOLD,                                                    &boundaryThreshold, "boundaryThreshold" },
                {input.noteThreshold,      schema.noteThreshold,      FALLBACK_NOTE_THRESHOLD,
                 &noteThreshold,                                                                                      "noteThreshold"     },
                {input.notePresenceCutoff, schema.notePresenceCutoff,
                 FALLBACK_NOTE_PRESENCE_CUTOFF,                                                  &settings.cutoff,   "notePresenceCutoff"},
                {input.boundaryRadius,     schema.boundaryRadius,     FALLBACK_BOUNDARY_RADIUS,
                 &radiusSeconds,                                                                                      "boundaryRadius"    },
            };
        for (const auto &[given, knob, fallback, target, what] : knobs) {
            auto chosen = otter::chooseKnob(given, knob, fallback, what);
            if (!chosen) {
                return chosen.takeError();
            }
            *target = chosen.take();
        }
        auto steps = otter::chooseKnob(input.steps, schema.steps, FALLBACK_STEPS, "steps");
        if (!steps) {
            return steps.takeError();
        }
        settings.steps = steps.take();
        settings.boundaryThreshold = static_cast<float>(boundaryThreshold);
        settings.noteThreshold = static_cast<float>(noteThreshold);
        // The engine counts the radius in its own frames. The contract states it in seconds so
        // that its meaning to a host is independent of the model's frame rate.
        settings.radius = std::max<int>(
            1, static_cast<int>(std::llround(radiusSeconds / configuration.timestep)));
        return settings;
    }

    /// Returns the model's number for the language of an execution.
    ///
    /// \return The model's number for the requested or default language; 0 if neither is specified
    ///         and the model has no language numbering;
    ///         \c srt::Error::InvalidArgument if neither is specified and the model numbers its
    ///         languages, or if the language is not declared or not numbered.
    srt::Expected<std::int64_t> chooseLanguage(const NoteApi::NoteStartInput &input,
                                               const NoteApi::NoteSchema &schema,
                                               const GameGgmlConfiguration &configuration) {
        const auto &wanted = input.language.value_or(schema.defaultLanguage);
        if (wanted.empty()) {
            if (!configuration.languages.empty()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this model numbers its languages, and neither the "
                                  "execution nor the declaration specifies a language");
            }
            return 0;
        }
        const auto &declared = schema.languages;
        if (std::find(declared.begin(), declared.end(), wanted) == declared.end()) {
            return srt::Error(srt::Error::InvalidArgument,
                              "the caller asked for the language " + wanted +
                                  ", which this model cannot transcribe");
        }
        const auto it = configuration.languages.find(wanted);
        if (it == configuration.languages.end()) {
            return srt::Error(srt::Error::InvalidArgument,
                              "this model has no number for the language " + wanted);
        }
        return it->second;
    }

    /// The request frame of one execution, as the engine's protocol reads it.
    std::string serveRequestHeader(std::int64_t languageId, const Settings &settings,
                                   std::size_t sampleCount) {
        std::string frame(SERVE_HEADER_SIZE, '\0');
        auto put = [&frame](std::size_t offset, const void *data, std::size_t size) {
            std::memcpy(frame.data() + offset, data, size);
        };
        const std::uint32_t magic = SERVE_MAGIC_INFERENCE;
        const std::int32_t language = static_cast<std::int32_t>(languageId);
        const std::uint64_t seed = FIXED_SEED;
        const std::int32_t steps = settings.steps;
        const std::int32_t radius = settings.radius;
        const std::uint32_t samples = static_cast<std::uint32_t>(sampleCount);
        put(0, &magic, sizeof(magic));
        put(4, &language, sizeof(language));
        put(8, &seed, sizeof(seed));
        put(16, &steps, sizeof(steps));
        put(20, &settings.boundaryThreshold, sizeof(settings.boundaryThreshold));
        put(24, &radius, sizeof(radius));
        put(28, &settings.noteThreshold, sizeof(settings.noteThreshold));
        put(32, &samples, sizeof(samples));
        return frame;
    }

    /// One line of the engine's output, as the protocol's JSON envelope.
    struct ServeResponse {
        bool ready = false;
        bool error = false;
        std::string message;

        /// The notes of a transcription, as the envelope carries them: offset, duration, pitch and
        /// the voiced flag of each.
        std::vector<std::tuple<double, double, double, bool>> notes;
    };

    /// Parses one response line of the engine.
    ///
    /// \return The response, or an \c AnalysisError::ModelFailed error if the line is not a JSON
    ///         object the protocol could have written.
    srt::Expected<ServeResponse> parseServeResponse(const std::string &line) {
        stdc::json::ParseError error;
        const auto value = stdc::json::Value::fromJson(line, false, &error);
        if (error || !value.isObject()) {
            return srt::Error(otter::AnalysisError::ModelFailed,
                              "the game CLI answered something the protocol does not define: " +
                                  line);
        }
        const auto object = value.toObject();
        const auto type = object.find("type");
        if (type == object.end() || !type->second.isString()) {
            return srt::Error(otter::AnalysisError::ModelFailed,
                              "the game CLI answered without a type: " + line);
        }
        ServeResponse response;
        const auto typeName = type->second.toString();
        if (typeName == "ready") {
            response.ready = true;
            return response;
        }
        if (typeName == "error") {
            response.error = true;
            if (const auto message = object.find("message"); message != object.end() &&
                                                             message->second.isString()) {
                response.message = message->second.toString();
            }
            return response;
        }
        if (typeName != "notes") {
            return srt::Error(otter::AnalysisError::ModelFailed,
                              "the game CLI answered with the unknown type " + typeName);
        }
        const auto notes = object.find("notes");
        if (notes == object.end() || !notes->second.isArray()) {
            return srt::Error(otter::AnalysisError::ModelFailed,
                              "the game CLI answered with notes but no note list");
        }
        for (const auto &note : notes->second.toArray()) {
            if (!note.isObject()) {
                return srt::Error(otter::AnalysisError::ModelFailed,
                                  "the game CLI answered with a note that is not an object");
            }
            const auto fields = note.toObject();
            const auto offset = fields.find("o");
            const auto duration = fields.find("d");
            const auto pitch = fields.find("p");
            const auto voiced = fields.find("v");
            if (offset == fields.end() || duration == fields.end() || pitch == fields.end() ||
                voiced == fields.end()) {
                return srt::Error(otter::AnalysisError::ModelFailed,
                                  "the game CLI answered with a note without its four fields");
            }
            response.notes.emplace_back(offset->second.toDouble(), duration->second.toDouble(),
                                        pitch->second.toDouble(), voiced->second.toInt() != 0);
        }
        return response;
    }

    class GameGgmlExecutive : public NoteApi::NoteExecutive {
    public:
        GameGgmlExecutive(srt::InferenceSpec &spec, const GameGgmlConfiguration &configuration,
                          const NoteApi::NoteSchema &schema)
            : NoteExecutive(spec), m_configuration(configuration), m_schema(schema) {
        }

        ~GameGgmlExecutive() override {
            // The body talks to the engine process, so an execution still in flight has to be
            // stopped and waited out before the process is torn down, which happens when the
            // members of this derived part are destroyed, before the base part and with it the
            // task.
            (void) stop();
            (void) waitForFinished();
            m_serve.terminate();
        }

        /// Requests cancellation and also kills the engine, because the request frame in flight is
        /// what a stopped execution is blocked on. The handle and the pipes are reaped by the
        /// execution thread, which is the only thread that owns them.
        srt::Expected<void> stop() override {
            (void) NoteExecutive::stop();
            (void) m_serve.requestCancel();
            return srt::Expected<void>();
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
            const auto &waveform = prepared.take();
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }
            auto language = chooseLanguage(input, m_schema, m_configuration);
            if (!language) {
                return language.takeError();
            }
            auto chosen = chooseSettings(input, m_schema, m_configuration);
            if (!chosen) {
                return chosen.takeError();
            }
            const auto settings = chosen.take();

            // The serve protocol runs no alignment model, so there is nothing to condition on. A
            // caller asking for it would otherwise be answered with free transcription, which
            // reads as success and silently ignores the score it was handed.
            if (!input.knownNotes.empty()) {
                return srt::Error(srt::Error::InvalidArgument,
                                  "this model cannot be conditioned on known notes");
            }

            const auto report = [this, &input](double value) {
                if (input.progress && !input.progress(value)) {
                    // The callback answered that the execution does not continue, so the stop is
                    // requested here and the call to checkCancelled() below reports it.
                    (void) stop();
                }
            };
            report(0);
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }

            auto produced = transcribe(waveform, *language, settings);
            if (!produced) {
                return produced.takeError();
            }
            if (auto stopped = checkCancelled(); !stopped) {
                return stopped.takeError();
            }

            auto result = layOut(produced.take(), audio.startTime, settings.cutoff);
            if (!result) {
                return result.takeError();
            }
            report(1);
            return result;
        }

    private:
        // Returns whether a stop has been requested for the current execution.
        srt::Expected<void> checkCancelled() const {
            if (cancelled()) {
                return otter::cancelledError();
            }
            return srt::Expected<void>();
        }

        // Spawns the engine, if no healthy process is held, and waits for its readiness.
        //
        // The process of a cancelled execution is terminated, so an analyzer that was stopped
        // simply relaunches its engine here; the model load is the engine's own business.
        srt::Expected<void> ensureEngine() {
            if (m_serve.alive()) {
                return srt::Expected<void>();
            }
            m_ready = false;
            auto spawned = m_serve.start(
                {m_configuration.cli.string(), "serve", m_configuration.model.string()});
            if (!spawned) {
                return spawned.takeError();
            }
            auto line = m_serve.readLine();
            if (!line) {
                m_serve.terminate();
                return srt::Error(otter::AnalysisError::ModelFailed,
                                  "the game CLI closed its output before announcing readiness");
            }
            auto response = parseServeResponse(line.take());
            if (!response) {
                m_serve.terminate();
                return response.takeError();
            }
            if (!response->ready) {
                m_serve.terminate();
                return srt::Error(otter::AnalysisError::ModelFailed,
                                  "the game CLI failed to load its model: " + response->message);
            }
            m_ready = true;
            return srt::Expected<void>();
        }

        // Runs one transcription request against the engine.
        //
        // The engine reports the failures of its own layer through the protocol's error envelope;
        // a process that dies mid-protocol is reported as a model failure, unless the execution
        // was cancelled, whose stop is the reason the process died.
        srt::Expected<std::vector<std::tuple<double, double, double, bool>>>
            transcribe(const otter::PreparedSamples &waveform, std::int64_t languageId,
                       const Settings &settings) {
            auto engine = ensureEngine();
            if (!engine) {
                return engine.takeError();
            }
            const auto request = serveRequestHeader(languageId, settings, waveform.size());
            auto written = m_serve.write(request.data(), request.size());
            if (written) {
                written = m_serve.write(waveform.data(), waveform.size() * sizeof(float));
            }
            if (!written) {
                return processLost("cannot hand the span to the game CLI");
            }
            auto line = m_serve.readLine();
            if (!line) {
                return processLost("the game CLI closed its output before answering");
            }
            auto response = parseServeResponse(line.take());
            if (!response) {
                return response.takeError();
            }
            if (response->error) {
                return srt::Error(otter::AnalysisError::ModelFailed,
                                  "the game CLI refused the execution: " + response->message);
            }
            return response->notes;
        }

        // Reports the loss of the engine process, as a cancellation when one was requested and as
        // a model failure otherwise.
        srt::Error processLost(const std::string &what) {
            m_serve.terminate();
            if (cancelled()) {
                return otter::cancelledError();
            }
            return srt::Error(otter::AnalysisError::ModelFailed, what);
        }

        // Lays the notes out on the host's timeline, dropping those below the cutoff.
        //
        // The engine states the presence of a note as a flag rather than a confidence. The
        // contract requires a confidence between zero and one, and a flag is a confidence with two
        // values: one for a voiced note and zero for a rest, which the cutoff drops.
        static srt::Expected<std::unique_ptr<NoteApi::NoteResult>>
            layOut(const std::vector<std::tuple<double, double, double, bool>> &produced,
                   double startTime, double cutoff) {
            auto result = std::make_unique<NoteApi::NoteResult>();
            for (const auto &[offset, duration, pitch, voiced] : produced) {
                const double confidence = voiced ? 1.0 : 0.0;
                if (confidence >= cutoff) {
                    result->notes.push_back(
                        {static_cast<int>(std::lround(pitch)), startTime + offset, duration,
                         confidence});
                }
            }
            return result;
        }

        // Spawned on demand and held for the lifetime of the analyzer, so the model loads once no
        // matter how many spans the host transcribes.
        otter::cli::Process m_serve;
        bool m_ready = false;

        // Owned by the spec, which outlives every executive created from it.
        const GameGgmlConfiguration &m_configuration;
        const NoteApi::NoteSchema &m_schema;
    };

    /// Spawns the engine on demand and hands out analyzers.
    class GameGgmlInterpreter : public otter::AnalysisInterpreter {
    public:
        GameGgmlInterpreter() = default;

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
            // The library checks the contract syntax. Whether this package can honor its
            // declaration is checked here, where both blocks are available. The model's own facts
            // live in its GGUF file, which no package load should pay for reading, so the audio
            // format is checked against the engine at the first execution instead.
            if (declared.channelCount != 1) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the game-ggml variant feeds its model one channel, and the "
                                  "exports declare " +
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
            // The serve protocol opens no session for the model that converts known durations into
            // boundaries, so the exports cannot promise the alignment path.
            if (declared.supportsKnownNotes) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the game-ggml variant cannot be conditioned on known notes, "
                                  "so the exports cannot declare supportsKnownNotes");
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
                spec.configuration() ? spec.configuration()->as<GameGgmlConfiguration>() : nullptr;
            const auto schema =
                spec.exports() ? spec.exports()->as<NoteApi::NoteSchema>() : nullptr;
            if (configuration == nullptr || schema == nullptr) {
                return srt::Error(otter::AnalysisError::Internal,
                                  "this declaration carries no game-ggml configuration");
            }
            auto checked = checkFiles(*configuration);
            if (!checked) {
                return checked.takeError();
            }
            return std::unique_ptr<srt::InferenceExecutive>(
                new GameGgmlExecutive(spec, *configuration, *schema));
        }

    private:
        // The engine and the model are files the declaration names. A package whose files are
        // missing is one that cannot run, however sound its declaration looks, and the refusal
        // belongs here, where the analyzer is created, rather than at the first execution.
        static srt::Expected<void> checkFiles(const GameGgmlConfiguration &configuration) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(configuration.cli, error)) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "cannot find the game CLI at " + configuration.cli.string());
            }
            if (!std::filesystem::is_regular_file(configuration.model, error)) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "cannot find the game model at " + configuration.model.string());
            }
            return srt::Expected<void>();
        }

        static srt::Expected<std::unique_ptr<GameGgmlConfiguration>>
            readConfiguration(const srt::ContribSpec &spec) {
            const auto &value = spec.manifestConfiguration();
            if (!value.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "the game-ggml configuration must be an object");
            }
            const auto object = value.toObject();
            if (auto checked =
                    otter::manifest::rejectUnknownKeys(object, {"cli", "model", "languages",
                                                                "timestep"},
                                                       "the game-ggml configuration");
                !checked) {
                return checked.takeError();
            }

            auto result = std::make_unique<GameGgmlConfiguration>();

            const auto directory = spec.declarationPath().parent_path();
            const std::pair<const char *, std::filesystem::path GameGgmlConfiguration::*> files[] = {
                {"cli",   &GameGgmlConfiguration::cli  },
                {"model", &GameGgmlConfiguration::model},
            };
            for (const auto &[key, member] : files) {
                const auto it = object.find(key);
                if (it == object.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      std::string("the game-ggml configuration needs a ") + key);
                }
                auto path = otter::manifest::readPath(it->second, directory, key);
                if (!path) {
                    return path.takeError();
                }
                result.get()->*member = path.take();
            }

            if (const auto it = object.find("timestep"); it != object.end()) {
                auto number = otter::manifest::readPositiveDouble(it->second, "timestep");
                if (!number) {
                    return number.takeError();
                }
                result->timestep = number.take();
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

    class GameGgmlPlugin : public srt::InferenceInterpreterPlugin {
    public:
        srt::Expected<std::unique_ptr<srt::ContribInterpreter>>
            create(std::string_view interfaceName, int level, std::string_view variant) override {
            if (interfaceName != NoteApi::API_INTERFACE || level != NoteApi::API_LEVEL ||
                variant != VARIANT) {
                return srt::Error(srt::Error::FeatureNotSupported,
                                  "this plugin serves the " + std::string(NoteApi::API_INTERFACE) +
                                      " contract at level " + std::to_string(NoteApi::API_LEVEL) +
                                      " only, in the variant " + VARIANT);
            }
            return std::unique_ptr<srt::ContribInterpreter>(new GameGgmlInterpreter());
        }
    };

}

STDC_EXPORT_PLUGIN(GameGgmlPlugin)
