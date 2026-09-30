// Includes the public headers available to a host and references one library symbol, so that the
// executable links against the library.

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
    return otter::analysisErrorCategory().name() == nullptr ? 1 : 0;
}
