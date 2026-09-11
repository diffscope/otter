#include <algorithm>
#include <string_view>
#include <vector>

#include <synthrt/Core/ContribCategory.h>
#include <synthrt/Core/SynthUnit.h>

#include <otter/Analysis/AnalysisContrib.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(test_AnalysisContrib)

BOOST_AUTO_TEST_CASE(test_AnalysisContrib_Registered) {
    std::vector<std::string_view> names;
    for (const auto &entry : srt::ContribCategoryRegistry::entries()) {
        names.push_back(entry.name());
    }

    BOOST_CHECK(std::find(names.begin(), names.end(), otter::ANALYSIS_CATEGORY) != names.end());
}

BOOST_AUTO_TEST_CASE(test_AnalysisContrib_BuiltByEveryUnit) {
    srt::SynthUnit unit;
    auto category = unit.category(otter::ANALYSIS_CATEGORY);
    BOOST_REQUIRE(category != nullptr);
    BOOST_CHECK_EQUAL(category->name(), otter::ANALYSIS_CATEGORY);
    BOOST_CHECK(&category->synthUnit() == &unit);
    BOOST_CHECK(category->as<otter::AnalysisCategory>()->analyzers().empty());

    srt::SynthUnit other;
    BOOST_CHECK(other.category(otter::ANALYSIS_CATEGORY) != nullptr);
    BOOST_CHECK(other.category(otter::ANALYSIS_CATEGORY) != category);
}

BOOST_AUTO_TEST_SUITE_END()
