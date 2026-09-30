// otter's analyzers are inference modules. Linking the library must not add a category, and a unit
// must provide the analyzers' category without any contribution from otter.

#include <algorithm>
#include <string_view>
#include <vector>

#include <synthrt/Core/ContribCategory.h>
#include <synthrt/Core/SynthUnit.h>
#include <synthrt/SVS/InferenceContrib.h>

#include <otter/Analysis/AnalysisError.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(test_AnalysisContrib)

BOOST_AUTO_TEST_CASE(test_AnalysisContrib_RegistersNoCategory) {
    // References a library symbol so that the library is linked into this test.
    BOOST_CHECK(otter::analysisErrorCategory().name() != nullptr);

    std::vector<std::string_view> names;
    for (const auto &entry : srt::ContribCategoryRegistry::entries()) {
        names.push_back(entry.name());
    }
    BOOST_CHECK(std::find(names.begin(), names.end(), "analysis") == names.end());

    srt::SynthUnit unit;
    BOOST_CHECK(unit.category("analysis") == nullptr);
    BOOST_CHECK(unit.category(srt::InferenceCategory::NAME) != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()
