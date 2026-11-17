#ifndef OTTER_TESTS_TESTSUPPORT_H
#define OTTER_TESTS_TESTSUPPORT_H

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace otter::test {

    /// Returns the error description of \a expected, or an empty string if \a expected holds a
    /// value.
    ///
    /// Calling error() on an Expected that holds a value reads the inactive member of a union, and
    /// toString() then traverses a nonexistent cause chain. Boost.Test evaluates the message
    /// argument of BOOST_REQUIRE_MESSAGE regardless of the outcome of the assertion, so the direct
    /// form <tt>BOOST_REQUIRE_MESSAGE(exp, exp.error().toString())</tt> makes this error on every
    /// passing assertion.
    template <class Expected>
    std::string why(const Expected &expected) {
        return expected ? std::string() : expected.error().toString();
    }

}

// The loading and runtime tests read a directory of packages that the tests treat as data rather
// than sources. scripts/make-test-packages.cmake rebuilds that directory, and the build passes its
// location as the OTTER_TEST_PACKAGES_SOURCE environment variable:
//
//     cmake -DOTTER_TEST_PACKAGES_OUTPUT=<dir> -P scripts/make-test-packages.cmake
//
// A run that is not told where the packages are reports that it did not run, which ctest shows as
// a skip. A run that is told but finds nothing there fails instead, because a directory that was
// named on purpose and is empty means the preparation step did not happen.
#ifndef OTTER_TEST_SKIP_EXIT_CODE
#  error "define OTTER_TEST_SKIP_EXIT_CODE for every test that includes TestSupport.h"
#endif

namespace otter::test {

    /// Returns the directory holding the packages that the loading and runtime tests read.
    inline std::filesystem::path packagesRoot() {
        const char *given = std::getenv("OTTER_TEST_PACKAGES_SOURCE");
        if (given == nullptr || *given == '\0') {
            std::cerr << "SKIP: no test packages; generate them with\n"
                         "    cmake -DOTTER_TEST_PACKAGES_OUTPUT=<dir> -P "
                         "scripts/make-test-packages.cmake\n"
                         "and point OTTER_TEST_PACKAGES_SOURCE at that directory\n";
            std::exit(OTTER_TEST_SKIP_EXIT_CODE);
        }
        const std::filesystem::path root(given);
        if (!std::filesystem::is_directory(root)) {
            std::cerr << "OTTER_TEST_PACKAGES_SOURCE is set to " << root.string()
                      << ", which is not a directory\n";
            std::exit(EXIT_FAILURE);
        }
        return root;
    }

}

// Helpers shared by the tests against the model fixtures. The build defines
// OTTER_TEST_FIXTURE_DIR for those tests only, and those tests include this header after
// Boost.Test.
#ifdef OTTER_TEST_FIXTURE_DIR

#  include <cstdlib>
#  include <filesystem>
#  include <iostream>
#  include <string_view>
#  include <utility>
#  include <vector>

#  include <boost/test/unit_test.hpp>

#  include <dsinfer/Api/Drivers/Onnx/OnnxDriverApi.h>
#  include <dsinfer/Inference/InferenceDriverFactory.h>

#  include <synthrt/Core/PackageHandle.h>
#  include <synthrt/Core/SynthUnit.h>
#  include <synthrt/SVS/InferenceContrib.h>
#  include <synthrt/Support/Error.h>

namespace otter::test {

    /// Ends the test run with the status that ctest reports as a skip if a prerequisite of the
    /// model tests is missing, so that such a run is reported as not run rather than as a success.
    ///
    /// The fixtures count as present only after the build has finished writing them, which the
    /// build marks with a stamp file. A directory left incomplete by an interrupted generation is
    /// therefore not mistaken for a complete directory, which would fail the tests instead of
    /// skipping them.
    struct FixturesOrSkip {
        FixturesOrSkip() {
            const auto stamp = std::filesystem::path(OTTER_TEST_FIXTURE_DIR) / ".stamp";
            if (!std::filesystem::is_regular_file(stamp)) {
                std::cerr << "SKIP: no complete model fixtures under " OTTER_TEST_FIXTURE_DIR "\n";
                std::exit(OTTER_TEST_SKIP_EXIT_CODE);
            }
            if (!std::filesystem::is_directory(
                    std::filesystem::path(OTTER_TEST_DRIVER_PLUGIN_DIR)) ||
                std::string_view(OTTER_TEST_ONNXRUNTIME_DIR).empty()) {
                std::cerr << "SKIP: no ONNX driver plugin or ONNX Runtime in this tree\n";
                std::exit(OTTER_TEST_SKIP_EXIT_CODE);
            }
        }
    };

    /// Test fixture that performs the setup of a host: it finds the ONNX driver, initializes the
    /// driver against a runtime directory named by the host, and registers the driver as the
    /// backend shared by the whole unit. The interpreter is the static plugin that the test
    /// registers, so the unit searches no plugin directory of the inference category.
    struct ModelHost {
        ModelHost() {
            std::vector<std::filesystem::path> driverPaths = {
                std::filesystem::path(OTTER_TEST_DRIVER_PLUGIN_DIR)};
            factory.setPluginPaths(driverPaths);
            auto loader = factory.find(ds::Api::Onnx::API_NAME);
            BOOST_REQUIRE_MESSAGE(loader != nullptr, "the ONNX driver plugin should be present");
            auto created = factory.create(loader);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(created), why(created));
            auto driver = created.take();
            ds::Api::Onnx::DriverInitArgs args;
            args.ep = ds::Api::Onnx::ExecutionProvider::CPU;
            args.runtimePath = std::filesystem::path(OTTER_TEST_ONNXRUNTIME_DIR);
            auto initialized = driver->initialize(args);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(initialized), why(initialized));
            BOOST_REQUIRE_MESSAGE(unit.addRuntimeService(std::move(driver)),
                                  "the driver should have been registered");
        }

        ~ModelHost() {
            package.reset();
        }

        /// Loads the fixture package \a name and returns its module \a contribution.
        srt::ContribSpec *load(const std::string &name, const char *contribution) {
            auto opened = unit.openPackage(std::filesystem::path(OTTER_TEST_FIXTURE_DIR) / name,
                                           srt::SynthUnit::Load);
            BOOST_REQUIRE_MESSAGE(static_cast<bool>(opened), why(opened));
            package = opened.take();
            auto spec = package.contribution(srt::InferenceCategory::NAME, contribution);
            BOOST_REQUIRE(spec != nullptr);
            return spec;
        }

        /// Returns the root cause of the load failure of the fixture package \a name, which the
        /// test expects to fail to load.
        ///
        /// A provider refuses a declaration inside its own interpreter, so the root cause is that
        /// refusal: its code says what the provider rejected the package as, and its text says why.
        srt::Error refusalError(const std::string &name) {
            auto opened = unit.openPackage(std::filesystem::path(OTTER_TEST_FIXTURE_DIR) / name,
                                           srt::SynthUnit::Load);
            BOOST_REQUIRE(!opened);
            return opened.error().rootCause();
        }

        /// Returns the text of the refusal that refusalError(\a name) returns, for the tests that
        /// assert on what a package was refused for and not on the kind of failure it is.
        std::string refusal(const std::string &name) {
            return refusalError(name).message();
        }

        ds::InferenceDriverFactory factory;
        srt::SynthUnit unit;
        srt::PackageHandle package;
    };

}

#endif // OTTER_TEST_FIXTURE_DIR

// The manifest of the plugin whose interpreter a test of tests/auto/plugins registers as a static
// plugin. The build defines OTTER_TEST_PLUGIN_MANIFEST for those tests only.
#ifdef OTTER_TEST_PLUGIN_MANIFEST

#  include <fstream>
#  include <iterator>

#  include <stdcorelib/support/json.h>

namespace otter::test {

    /// Returns the plugin.json of the plugin under test, or a null value if the file does not
    /// parse. Discovery selects no interpreter for a null manifest, so a broken manifest fails the
    /// loads of the test.
    inline stdc::json::Value pluginManifest() {
        std::ifstream file(std::filesystem::path(OTTER_TEST_PLUGIN_MANIFEST), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
        return stdc::json::Value::fromJson(text, false);
    }

}

#endif // OTTER_TEST_PLUGIN_MANIFEST

// The fake engine that the tests of the ggml providers run in place of the command line tools of
// the engines. The build defines OTTER_TEST_FAKE_CLI for those tests only.
#ifdef OTTER_TEST_FAKE_CLI

#  include <chrono>
#  include <stdlib.h>
#  include <system_error>
#  include <thread>
#  include <utility>

namespace otter::test {

    /// Makes the fake engines started while this object lives hang. Such an engine takes a request
    /// and never answers, after it has created the marker file that the environment variable
    /// OTTER_FAKE_CLI_HANG names, from which the test learns that the engine hangs. An engine reads
    /// the environment that it inherits at its start.
    class HangingEngine {
    public:
        explicit HangingEngine(std::filesystem::path marker) : m_marker(std::move(marker)) {
            std::error_code ignored;
            std::filesystem::remove(m_marker, ignored);
            set(m_marker.string().c_str());
        }

        ~HangingEngine() {
            set(nullptr);
            std::error_code ignored;
            std::filesystem::remove(m_marker, ignored);
        }

        HangingEngine(const HangingEngine &) = delete;
        HangingEngine &operator=(const HangingEngine &) = delete;

        /// Waits at most \a timeout for an engine to hang and returns whether one does.
        bool waitUntilHanging(std::chrono::milliseconds timeout) const {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!std::filesystem::exists(m_marker)) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return true;
        }

    private:
        static void set(const char *value) {
#  ifdef _WIN32
            (void) _putenv_s("OTTER_FAKE_CLI_HANG", value ? value : "");
#  else
            if (value) {
                (void) setenv("OTTER_FAKE_CLI_HANG", value, 1);
            } else {
                (void) unsetenv("OTTER_FAKE_CLI_HANG");
            }
#  endif
        }

        std::filesystem::path m_marker;
    };

}

#endif // OTTER_TEST_FAKE_CLI

#endif // OTTER_TESTS_TESTSUPPORT_H
