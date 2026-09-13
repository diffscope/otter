#include <otter/Analysis/AnalysisContrib.h>

#include <set>
#include <string_view>

#include <otter/Analysis/AnalysisProviderPlugin.h>

namespace otter {

    namespace {

        /// The contribution entry in desc.json, whose whole schema this category owns.
        ///
        /// Strict here and lenient in the declaration root, which is the split spec 2.4 asks for:
        /// the root is an object the upper specification defines and may extend, this entry is not.
        srt::Expected<void> validateEntry(const srt::JsonObject &entry) {
            static const std::set<std::string_view> fields = {"id", "path"};
            for (const auto &item : entry) {
                if (fields.find(item.first) == fields.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      "analysis contribution entry has an unknown field: " +
                                          item.first);
                }
            }
            return {};
        }

    }

    AnalysisSpec::AnalysisSpec(const srt::ContribCreateContext &context) : ContribSpec(context) {
    }

    AnalysisSpec::~AnalysisSpec() = default;

    AnalysisCategory::AnalysisCategory()
        : ContribCategory(ANALYSIS_CATEGORY, ModuleDeclaration, AnalysisProviderPlugin::IID) {
    }

    AnalysisCategory::~AnalysisCategory() = default;

    std::vector<AnalysisSpec *> AnalysisCategory::analyzers() const {
        std::vector<AnalysisSpec *> result;
        const auto values = contributions();
        result.reserve(values.size());
        for (auto value : values) {
            result.push_back(value->as<AnalysisSpec>());
        }
        return result;
    }

    srt::Expected<std::unique_ptr<srt::ContribSpec>>
        AnalysisCategory::createSpec(const srt::ContribCreateContext &context) const {
        if (auto result = validateEntry(context.manifestEntry()); !result) {
            return result.takeError();
        }
        if (!context.manifestDeclaration() || !context.declarationPath()) {
            return srt::Error(srt::Error::InvalidFormat,
                              "analysis contribution requires a declaration");
        }
        // The category adds no fields, so there is nothing further to read here. Everything an
        // analysis declaration says beyond the common fields is the contract's business, and the
        // framework has already parsed the common fields by the time this runs.
        return std::unique_ptr<srt::ContribSpec>(new AnalysisSpec(context));
    }

}

namespace otter {

    void linkAnalysisCategory() noexcept {
    }

}

static srt::ContribCategoryRegistry::Add<otter::AnalysisCategory>
    analysisCategoryRegistration(otter::ANALYSIS_CATEGORY, "");
