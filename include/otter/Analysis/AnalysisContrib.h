#ifndef OTTER_ANALYSISCONTRIB_H
#define OTTER_ANALYSISCONTRIB_H

#include <memory>
#include <vector>

#include <synthrt/Core/ContribCategory.h>
#include <synthrt/Core/ContribSpec.h>

#include <otter/otter_global.h>

namespace otter {

    inline constexpr char ANALYSIS_CATEGORY[] = "analysis";

    /// The immutable declaration of one analysis contribution.
    ///
    /// The category adds no fields of its own. Everything a host needs before it picks an
    /// analyzer — the display name, the contract it implements, the variant — is already in the
    /// common module declaration, so there is nothing for this class to parse that the framework
    /// has not parsed. It exists to give the category a spec type of its own, which is what the
    /// extension traits key on.
    class OTTER_EXPORT AnalysisSpec : public srt::ContribSpec {
    public:
        ~AnalysisSpec();

    private:
        explicit AnalysisSpec(const srt::ContribCreateContext &context);

        friend class AnalysisCategory;
    };

    /// Parses and indexes contributions in the otter analysis category.
    class OTTER_EXPORT AnalysisCategory : public srt::ContribCategory {
    public:
        AnalysisCategory();
        ~AnalysisCategory();

        /// Returns all committed analysis contributions.
        std::vector<AnalysisSpec *> analyzers() const;

    protected:
        srt::Expected<std::unique_ptr<srt::ContribSpec>>
            createSpec(const srt::ContribCreateContext &context) const override;
    };

}

#endif // OTTER_ANALYSISCONTRIB_H
