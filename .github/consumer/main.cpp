// Includes the public headers available to a host, references library symbols so that the
// executable links against the library, and checks the plugin category directory that the build
// reads from an exported plugin target.

#include <cstdio>
#include <filesystem>

#include <synthrt/Core/SynthUnit.h>
#include <synthrt/SVS/InferenceContrib.h>

#include <otter/Analysis/AnalysisError.h>
#include <otter/Api/Align/1/AlignApiL1.h>
#include <otter/Api/F0/1/F0ApiL1.h>
#include <otter/Api/Note/1/NoteApiL1.h>

int main() {
    // The analyzers are inference modules and therefore require only synthrt's inference category.
    srt::SynthUnit unit;
    if (unit.category(srt::InferenceCategory::NAME) == nullptr) {
        return 1;
    }
    // References one symbol defined in the library.
    if (otter::analysisErrorCategory().name() == nullptr) {
        return 1;
    }

    const auto plugins = std::filesystem::path(OTTER_CONSUMER_PLUGIN_DIR).lexically_normal();
    if (!std::filesystem::is_directory(plugins)) {
        std::fprintf(stderr, "the otter plugin directory %s does not exist\n",
                     plugins.string().c_str());
        return 1;
    }
    std::printf("otter plugins: %s\n", plugins.string().c_str());
    return 0;
}
