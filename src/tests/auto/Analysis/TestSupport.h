#ifndef OTTER_TEST_SUPPORT_H
#define OTTER_TEST_SUPPORT_H

#include <string>

namespace otter::test {

    /// Returns the error description of \a expected, or an empty string if \a expected holds a
    /// value.
    ///
    /// Calling error() on an Expected that holds a value reads the inactive member of a union, and
    /// toString() then traverses a nonexistent cause chain. Boost.Test evaluates the message
    /// argument of BOOST_REQUIRE_MESSAGE regardless of the outcome of the assertion, so the direct
    /// form `BOOST_REQUIRE_MESSAGE(exp, exp.error().toString())` makes this error on every passing
    /// assertion.
    template <class Expected>
    std::string why(const Expected &expected) {
        return expected ? std::string() : expected.error().toString();
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
    /// driver against a runtime directory named by the host, registers the driver as the backend
    /// shared by the whole unit, and points the inference category at otter's interpreters.
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

            std::vector<std::filesystem::path> pluginPaths = {
                std::filesystem::path(OTTER_TEST_PLUGIN_DIR)};
            unit.setPluginPaths(srt::InferenceCategory::NAME, pluginPaths);
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
        std::string refusal(const std::string &name) {
            auto opened = unit.openPackage(std::filesystem::path(OTTER_TEST_FIXTURE_DIR) / name,
                                           srt::SynthUnit::Load);
            BOOST_REQUIRE(!opened);
            return opened.error().rootCause().message();
        }

        ds::InferenceDriverFactory factory;
        srt::SynthUnit unit;
        srt::PackageHandle package;
    };

}

#endif // OTTER_TEST_FIXTURE_DIR

#endif // OTTER_TEST_SUPPORT_H
