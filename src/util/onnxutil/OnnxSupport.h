#ifndef OTTER_ONNXUTIL_ONNXSUPPORT_H
#define OTTER_ONNXUTIL_ONNXSUPPORT_H

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <synthrt/SVS/InferenceContrib.h>
#include <synthrt/Support/Expected.h>
#include <synthrt/Support/Json.h>

#include <dsinfer/Core/Tensor.h>
#include <dsinfer/Inference/InferenceSession.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Analysis/Provider/AnalysisInput.h>

/// Code shared by the four ONNX providers: the sessions that an analyzer owns, the part of the
/// executive interface that concerns them, the driver lookup, the files a model states its facts
/// in, a single model call with the readers of its outputs, and the plugin's contract check.
///
/// This namespace is implemented by a private static library of this tree that each ONNX provider
/// links. It cannot reside in the otter library because the otter library does not depend on
/// dsinfer.
namespace otter::onnx {

    using TensorPtr = std::shared_ptr<ds::ITensor>;

    /// Tensors by name, as a session takes and returns them.
    using Tensors = std::map<std::string, TensorPtr>;

    /// Checks that the \a requested triplet is the triplet that the plugin serves, \a served.
    ///
    /// \return An empty value if the triplets are equal. Otherwise
    ///         \c srt::Error::FeatureNotSupported.
    srt::Expected<void> checkServed(const srt::ContribContractTriplet &requested,
                                    const srt::ContribContractTriplet &served);

    /// Opens the model at \a path through the ONNX driver registered on the unit of \a spec.
    ///
    /// \a what names the model in the error, as in <tt>rmvpe model</tt>. \a useCpu keeps the model
    /// on the CPU, which a variant asks for when the model is small and its work is what an
    /// accelerator backend does not carry.
    ///
    /// \return The open session; \c srt::Error::FeatureNotSupported if the driver is not
    ///         registered or creates no session. The driver's error if the model cannot be opened.
    srt::Expected<std::unique_ptr<ds::InferenceSession>>
        openSession(srt::InferenceSpec &spec, const std::filesystem::path &path,
                    std::string_view what, bool useCpu = false);

    /// Reads the file paths of a configuration block, each relative to \a directory.
    ///
    /// \a paths maps each key to the member it is read into. An absent key listed in \a required
    /// is rejected with \c srt::Error::InvalidFormat, and any other absent key leaves its member
    /// empty. \a what names the block in the error.
    srt::Expected<void>
        readPaths(const srt::JsonObject &object, const std::filesystem::path &directory,
                  std::initializer_list<std::pair<const char *, std::filesystem::path *>> paths,
                  std::initializer_list<const char *> required, std::string_view what);

    /// Reads a whole file as text.
    ///
    /// \return The text; \c srt::Error::FileNotOpen if the file cannot be opened;
    ///         \c srt::Error::InvalidFormat if the file is empty. \a what names the file in the
    ///         error.
    srt::Expected<std::string> readTextFile(const std::filesystem::path &path,
                                            std::string_view what);

    /// Reads one of the model's own JSON files, as an object.
    ///
    /// These files belong to the model rather than to the package, and they are read with the
    /// spec's profile (UTF-8, comments allowed, repeated keys rejected). A document that is not an
    /// object is rejected instead of being read as empty.
    ///
    /// \return The object, or the error of the first failed step.
    srt::Expected<srt::JsonValue> readJsonObject(const std::filesystem::path &path,
                                                 std::string_view what);

    /// Builds the [1, samples] float tensor every provider feeds its first model.
    srt::Expected<TensorPtr> waveformTensor(const PreparedSamples &samples);

    /// Copies a float tensor into a vector.
    ///
    /// \return The tensor's values, or \c AnalysisError::ModelFailed if \a tensor is null and
    ///         \c AnalysisError::ModelMismatch if it is not a float tensor. \a what names the
    ///         output in the error.
    srt::Expected<std::vector<float>> readFloats(const TensorPtr &tensor, const char *what);

    /// Runs one model and returns the requested outputs.
    ///
    /// A stop interrupts a running model by stopping its session, and the call then fails with
    /// the driver's error for an interrupted session. If \a cancelled returns true, that failure
    /// is reported as a cancellation instead of a model failure. A call that completed after the
    /// stop is also reported as a cancellation, because a cancelled execution reports no result.
    /// A missing output is reported as \c AnalysisError::ModelFailed. \a what names the model in
    /// errors.
    srt::Expected<Tensors> run(ds::InferenceSession &session, Tensors inputs,
                               const std::set<std::string> &wanted, std::string_view what,
                               const std::function<bool()> &cancelled);

    /// The executive of a provider whose models run in ONNX sessions.
    ///
    /// \a Contract is the contract's executive class. This class owns the sessions and forwards a
    /// stop and a wait to them after the contract's own quit and wait. A derived executive passes
    /// its sessions to own(). Its destructor calls quit() and wait() before closing the sessions
    /// and before any member that its body reads is destroyed.
    template <class Contract>
    class OnnxExecutive : public Contract {
    public:
    protected:
        /// Requests cancellation and also stops every session, because a session inside a forward
        /// pass is the component that blocks. A session without a running pass returns an error
        /// that indicates only this condition. The error is therefore not reported as a failed
        /// stop.
        ///
        /// \return An empty value in every case.
        srt::Expected<void> quit() override {
            (void) Contract::quit();
            for (const auto &session : m_sessions) {
                (void) session->stop();
            }
            return srt::Expected<void>();
        }

        /// Waits for the execution, then for every session. Every session is waited on even after
        /// another session reports a failure, because the purpose of this call is to leave no
        /// session running.
        ///
        /// \return An empty value if every session finished. Otherwise the first session failure.
        srt::Expected<void> wait() override {
            (void) Contract::wait();
            std::optional<srt::Error> failure;
            for (const auto &session : m_sessions) {
                if (auto waited = session->waitForFinished(); !waited && !failure) {
                    failure = waited.error();
                }
            }
            if (failure) {
                return *failure;
            }
            return srt::Expected<void>();
        }

        explicit OnnxExecutive(srt::InferenceSpec &spec) : Contract(spec) {
        }

        /// Takes ownership of \a session and returns it for the body to run, or null for an
        /// optional model that was not opened.
        ds::InferenceSession *own(std::unique_ptr<ds::InferenceSession> session) {
            if (!session) {
                return nullptr;
            }
            m_sessions.push_back(std::move(session));
            return m_sessions.back().get();
        }

        /// Closes every session after quit() and wait() have left no forward pass in progress.
        void closeSessions() {
            for (const auto &session : m_sessions) {
                (void) session->close();
            }
        }

        /// Converts a stop request into the error that the contract requires an execution to
        /// report.
        ///
        /// \return The cancellation error if a stop was requested. Otherwise an empty value.
        srt::Expected<void> checkCancelled() const {
            if (this->cancelled()) {
                return cancelledError();
            }
            return srt::Expected<void>();
        }

        /// Runs one of this executive's models, as run() does, with this execution's stop flag.
        srt::Expected<Tensors> runModel(ds::InferenceSession &session, Tensors inputs,
                                        const std::set<std::string> &wanted,
                                        std::string_view what) const {
            // Qualified: the contract's run() would hide this free function (the appendix of
            // docs/plans/tifa-align.md).
            return otter::onnx::run(session, std::move(inputs), wanted, what,
                                    [this] { return this->cancelled(); });
        }

    private:
        std::vector<std::unique_ptr<ds::InferenceSession>> m_sessions;
    };

}

#endif // OTTER_ONNXUTIL_ONNXSUPPORT_H
