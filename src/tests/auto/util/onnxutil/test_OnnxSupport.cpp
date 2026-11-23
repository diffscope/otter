#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>

#include <dsinfer/Api/Drivers/Onnx/OnnxDriverApi.h>
#include <dsinfer/Core/Tensor.h>

#include <otter/Analysis/AnalysisError.h>

#include <onnxutil/OnnxSupport.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace OnnxApi = ds::Api::Onnx;

namespace {

    // A session that exercises the executor without a model, since a session is the whole surface
    // that run() reads its outputs from. The caller selects what a call reports through completed
    // and result, which start() reaches only after the executor has accepted the request.
    struct StubSession : ds::InferenceSession {
        // The result that start() reports while completed is set, which is empty by default.
        OnnxApi::SessionResult result;

        // Whether start() reports a result at all. A false value leaves the execution successful
        // and its result absent, the state of an execution that produced nothing.
        bool completed = true;

        srt::Expected<void> open(const std::filesystem::path &,
                                 const ds::InferenceSessionOpenArgs &) override {
            return srt::Expected<void>();
        }

        srt::Expected<void> close() override {
            return srt::Expected<void>();
        }

        bool isOpen() const override {
            return true;
        }

        int64_t id() const override {
            return 0;
        }

    protected:
        srt::Expected<std::unique_ptr<srt::TaskResult>>
            execute(const srt::TaskStartInput &, const srt::TaskStopToken &) override {
            if (!completed) {
                return std::unique_ptr<srt::TaskResult>();
            }
            // The result cannot leave the session by value: TaskPayload deletes its copy, so no
            // move in the chain is usable either. The reported result is therefore built in place,
            // holding the outputs this session was given, whose tensors the map shares. Only the
            // pointer to it is moved.
            auto started = std::make_unique<OnnxApi::SessionResult>();
            started->outputs = result.outputs;
            return std::move(started);
        }
    };

}

BOOST_AUTO_TEST_SUITE(test_OnnxSupport)

// An output the model did not produce and an output the variant cannot read are different faults,
// and only the second one says that the model ran and produced something of the wrong element
// type. The two therefore have to leave the reader as different codes rather than as one failure.
BOOST_AUTO_TEST_CASE(test_OnnxSupport_ReportsAMissingAndAnUnreadableOutputDifferently) {
    // The control first: a float output is read back with its values. A reader that refused every
    // tensor would satisfy the two cases below without reading anything.
    auto floats = ds::Tensor::createFilled<float>({2}, 2.0f);
    BOOST_REQUIRE(floats);
    auto read = otter::onnx::readFloats(floats.take(), "f0 output");
    BOOST_REQUIRE(read);
    BOOST_CHECK_EQUAL(read->size(), 2u);
    BOOST_CHECK_EQUAL((*read)[1], 2.0f);

    // The model ran but named no such output, so the variant has nothing to read and the fault
    // lies with the model's own output set.
    auto missing = otter::onnx::readFloats(nullptr, "f0 output");
    BOOST_REQUIRE(!missing);
    BOOST_CHECK(missing.error().code() == otter::AnalysisError::ModelFailed);
    BOOST_CHECK(missing.error().message().find("f0 output") != std::string::npos);

    // The output exists and cannot be read as a waveform, which is a mismatch between the model
    // and the variant rather than a missing output of the model.
    auto wrongType = ds::Tensor::createScalar<std::int64_t>(1);
    BOOST_REQUIRE(wrongType);
    auto wrong = otter::onnx::readFloats(wrongType.take(), "f0 output");
    BOOST_REQUIRE(!wrong);
    BOOST_CHECK(wrong.error().code() == otter::AnalysisError::ModelMismatch);
    BOOST_CHECK(wrong.error().message().find("f0 output") != std::string::npos);
}

// An execution that completed without producing a result leaves the executor with nothing to
// read, which is a fault of the model's execution. The call has to refuse rather than report the
// empty output set that it never received.
BOOST_AUTO_TEST_CASE(test_OnnxSupport_RefusesAnExecutionThatReturnedNoResult) {
    StubSession session;
    session.completed = false;

    auto ran = otter::onnx::run(session, otter::onnx::Tensors(), std::set<std::string>{"f0"},
                                "rmvpe model", [] { return false; });
    BOOST_REQUIRE(!ran);
    BOOST_CHECK(ran.error().code() == otter::AnalysisError::ModelFailed);
    BOOST_CHECK_EQUAL(ran.error().message(), "the rmvpe model returned no outputs");
}

// A result that is present but does not hold one of the requested names is a fault of the model
// as well, and the message has to name the output that is missing so that the fault can be traced
// to the model rather than to the variant. The executor returns as soon as one requested name is
// absent, so the test requests one name.
BOOST_AUTO_TEST_CASE(test_OnnxSupport_RefusesAnOutputTheModelDidNotReturn) {
    StubSession session;

    auto ran = otter::onnx::run(session, otter::onnx::Tensors(), std::set<std::string>{"f0"},
                                "rmvpe model", [] { return false; });
    BOOST_REQUIRE(!ran);
    BOOST_CHECK(ran.error().code() == otter::AnalysisError::ModelFailed);
    BOOST_CHECK_EQUAL(ran.error().message(), "the rmvpe model did not return f0");

    // The requested name is present but carries no tensor, which is the same fault of the model
    // and has to report the same message rather than reach the readers with a null tensor.
    session.result.outputs.emplace("f0", nullptr);
    auto empty = otter::onnx::run(session, otter::onnx::Tensors(), std::set<std::string>{"f0"},
                                  "rmvpe model", [] { return false; });
    BOOST_REQUIRE(!empty);
    BOOST_CHECK(empty.error().code() == otter::AnalysisError::ModelFailed);
    BOOST_CHECK_EQUAL(empty.error().message(), "the rmvpe model did not return f0");
}

BOOST_AUTO_TEST_SUITE_END()
