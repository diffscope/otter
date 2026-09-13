#include <otter/Api/F0/1/F0ApiL1.h>

#include <string>
#include <utility>

#include <synthrt/Support/JSON.h>

#include <otter/Support/ManifestValues.h>

namespace otter::Api::F0::L1 {

    srt::Expected<std::unique_ptr<F0Schema>> readF0Schema(const srt::ContribSpec &spec,
                                                          std::string variant) {
        const auto &value = spec.manifestExports();
        if (!value.isObject()) {
            return srt::Error(srt::Error::InvalidFormat,
                              "an F0 declaration needs an exports object stating the audio "
                              "format it takes and the knobs it honors");
        }
        const auto object = value.toObject();
        if (auto checked = manifest::rejectUnknownKeys(
                object, {"sampleRate", "channelCount", "interval", "maxSegmentDuration", "knobs"},
                "the F0 exports");
            !checked) {
            return checked.takeError();
        }

        auto result = std::make_unique<F0Schema>(std::move(variant));

        const auto rate = object.find("sampleRate");
        if (rate == object.end()) {
            return srt::Error(srt::Error::InvalidFormat, "the F0 exports need a sampleRate");
        }
        if (auto number = manifest::readPositiveInt(rate->second, "sampleRate"); number) {
            result->sampleRate = number.take();
        } else {
            return number.takeError();
        }

        const auto interval = object.find("interval");
        if (interval == object.end()) {
            return srt::Error(srt::Error::InvalidFormat, "the F0 exports need an interval");
        }
        if (auto number = manifest::readPositiveDouble(interval->second, "interval"); number) {
            result->interval = number.take();
        } else {
            return number.takeError();
        }

        if (const auto it = object.find("channelCount"); it != object.end()) {
            auto number = manifest::readPositiveInt(it->second, "channelCount");
            if (!number) {
                return number.takeError();
            }
            result->channelCount = number.take();
        }
        if (const auto it = object.find("maxSegmentDuration"); it != object.end()) {
            auto number = manifest::readPositiveDouble(it->second, "maxSegmentDuration");
            if (!number) {
                return number.takeError();
            }
            result->maxSegmentDuration = number.take();
        }

        if (const auto it = object.find("knobs"); it != object.end()) {
            if (!it->second.isObject()) {
                return srt::Error(srt::Error::InvalidFormat, "knobs must be an object");
            }
            const auto knobs = it->second.toObject();
            if (auto checked = manifest::rejectUnknownKeys(
                    knobs, {"voicingThreshold", "interpolateUnvoiced"}, "the F0 knobs");
                !checked) {
                return checked.takeError();
            }
            if (const auto knob = knobs.find("voicingThreshold"); knob != knobs.end()) {
                auto read = manifest::readKnob(knob->second, "voicingThreshold");
                if (!read) {
                    return read.takeError();
                }
                result->voicingThreshold = read.take();
            }
            if (const auto knob = knobs.find("interpolateUnvoiced"); knob != knobs.end()) {
                auto read = manifest::readFlagKnob(knob->second, "interpolateUnvoiced");
                if (!read) {
                    return read.takeError();
                }
                result->interpolateUnvoiced = read.take();
            }
        }
        return result;
    }

}
