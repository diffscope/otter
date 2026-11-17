#ifndef OTTER_TESTS_PLUGINS_STUB_STUBINTERPRETERS_H
#define OTTER_TESTS_PLUGINS_STUB_STUBINTERPRETERS_H

#include <memory>

#include <otter/Analysis/Provider/AnalysisInterpreter.h>

namespace otter::stub {

    inline constexpr char VARIANT[] = "stub";

    /// Interprets F0 modules of the stub variant.
    class StubF0Interpreter : public otter::AnalysisInterpreter {
    public:
        StubF0Interpreter() = default;
        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override;
    };

    /// Interprets Note modules of the stub variant.
    class StubNoteInterpreter : public otter::AnalysisInterpreter {
    public:
        StubNoteInterpreter() = default;
        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override;
    };

    /// Interprets Align modules of the stub variant.
    class StubAlignInterpreter : public otter::AnalysisInterpreter {
    public:
        StubAlignInterpreter() = default;
        srt::Expected<std::unique_ptr<srt::ContribExports>>
            createExports(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::ContribConfiguration>>
            createConfiguration(const srt::ContribSpec &spec) const override;
        srt::Expected<std::unique_ptr<srt::InferenceExecutive>>
            createInference(srt::InferenceSpec &spec, const srt::ContribImportOptions &,
                            const srt::InferenceRuntimeOptions &) override;
    };

}

#endif // OTTER_TESTS_PLUGINS_STUB_STUBINTERPRETERS_H
