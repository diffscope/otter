#include <filesystem>

#include <stdcorelib/plugin/pluginloader.h>

#include <synthrt/Core/ContribInterpreterPlugin.h>
#include <synthrt/SVS/InferenceInterpreterPlugin.h>
#include <synthrt/SVS/SingerProviderPlugin.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace {

    void checkPlugin(const std::filesystem::path &file, const std::filesystem::path &metadata,
                     const char *iid) {
        stdc::plugin::PluginLoader loader(file, metadata);
        BOOST_REQUIRE(loader.state() == stdc::plugin::PluginLoader::Read);
        auto declarations =
            srt::ContribInterpreterPlugin::readInterpreterDeclarations(loader.metadata());
        BOOST_REQUIRE(declarations);
        BOOST_CHECK(!declarations.value().empty());
        BOOST_REQUIRE_MESSAGE(loader.load(), loader.errorMessage());
        BOOST_CHECK_EQUAL(loader.iid(), iid);
        auto plugin = static_cast<srt::ContribInterpreterPlugin *>(loader.plugin());
        BOOST_REQUIRE(plugin);
        for (const auto &triplet : declarations.value()) {
            auto created = plugin->create(triplet);
            BOOST_REQUIRE(created);
            BOOST_REQUIRE(created.value());
            auto otherLevel =
                plugin->create({triplet.interfaceId(), triplet.level() + 1, triplet.variant()});
            BOOST_REQUIRE(!otherLevel);
            auto otherVariant =
                plugin->create({triplet.interfaceId(), triplet.level(), "com.example.unsupported"});
            BOOST_REQUIRE(!otherVariant);
            auto otherInterface =
                plugin->create({"com.example.Unsupported", triplet.level(), triplet.variant()});
            BOOST_REQUIRE(!otherInterface);
        }
    }

}

#ifdef OTTER_TEST_OTTERRMVPE_FILE
BOOST_AUTO_TEST_CASE(otterrmvpeFactoryMatchesItsManifest) {
    checkPlugin(OTTER_TEST_OTTERRMVPE_FILE, OTTER_TEST_OTTERRMVPE_MANIFEST,
                srt::InferenceInterpreterPlugin::IID);
}
#endif

#ifdef OTTER_TEST_OTTERGAME_FILE
BOOST_AUTO_TEST_CASE(ottergameFactoryMatchesItsManifest) {
    checkPlugin(OTTER_TEST_OTTERGAME_FILE, OTTER_TEST_OTTERGAME_MANIFEST,
                srt::InferenceInterpreterPlugin::IID);
}
#endif

#ifdef OTTER_TEST_OTTERHFA_FILE
BOOST_AUTO_TEST_CASE(otterhfaFactoryMatchesItsManifest) {
    checkPlugin(OTTER_TEST_OTTERHFA_FILE, OTTER_TEST_OTTERHFA_MANIFEST,
                srt::InferenceInterpreterPlugin::IID);
}
#endif

#ifdef OTTER_TEST_OTTERTIFA_FILE
BOOST_AUTO_TEST_CASE(ottertifaFactoryMatchesItsManifest) {
    checkPlugin(OTTER_TEST_OTTERTIFA_FILE, OTTER_TEST_OTTERTIFA_MANIFEST,
                srt::InferenceInterpreterPlugin::IID);
}
#endif

#ifdef OTTER_TEST_OTTERGAMEGGML_FILE
BOOST_AUTO_TEST_CASE(ottergameggmlFactoryMatchesItsManifest) {
    checkPlugin(OTTER_TEST_OTTERGAMEGGML_FILE, OTTER_TEST_OTTERGAMEGGML_MANIFEST,
                srt::InferenceInterpreterPlugin::IID);
}
#endif

#ifdef OTTER_TEST_OTTERTIFAGGML_FILE
BOOST_AUTO_TEST_CASE(ottertifaggmlFactoryMatchesItsManifest) {
    checkPlugin(OTTER_TEST_OTTERTIFAGGML_FILE, OTTER_TEST_OTTERTIFAGGML_MANIFEST,
                srt::InferenceInterpreterPlugin::IID);
}
#endif
