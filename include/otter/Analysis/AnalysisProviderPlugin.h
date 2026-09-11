#ifndef OTTER_ANALYSISPROVIDERPLUGIN_H
#define OTTER_ANALYSISPROVIDERPLUGIN_H

#include <synthrt/Core/ContribInterpreterPlugin.h>

#include <otter/otter_global.h>

namespace otter {

    /// Creates providers for supported analysis contracts.
    class OTTER_EXPORT AnalysisProviderPlugin : public srt::ContribInterpreterPlugin {
    public:
        static constexpr const char *IID = "org.openvpi.otter.plugin.AnalysisProvider";

        ~AnalysisProviderPlugin() = default;

    protected:
        AnalysisProviderPlugin() = default;
    };

}

#endif // OTTER_ANALYSISPROVIDERPLUGIN_H
