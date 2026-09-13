#include <otter/Api/Note/1/NoteApiL1.h>

#include <string>
#include <utility>

#include <synthrt/Support/JSON.h>

#include <otter/Support/ManifestValues.h>

namespace otter::Api::Note::L1 {

    srt::Expected<std::unique_ptr<NoteSchema>> readNoteSchema(const srt::ContribSpec &spec,
                                                              std::string variant) {
        const auto &value = spec.manifestExports();
        if (!value.isObject()) {
            return srt::Error(srt::Error::InvalidFormat,
                              "a Note declaration needs an exports object stating the audio "
                              "format it takes and the knobs it honors");
        }
        const auto object = value.toObject();
        if (auto checked =
                manifest::rejectUnknownKeys(object,
                                            {"sampleRate", "channelCount", "maxSegmentDuration",
                                             "languages", "supportsKnownNotes", "knobs"},
                                            "the Note exports");
            !checked) {
            return checked.takeError();
        }

        auto result = std::make_unique<NoteSchema>(std::move(variant));

        const auto rate = object.find("sampleRate");
        if (rate == object.end()) {
            return srt::Error(srt::Error::InvalidFormat, "the Note exports need a sampleRate");
        }
        if (auto number = manifest::readPositiveInt(rate->second, "sampleRate"); number) {
            result->sampleRate = number.take();
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
        if (const auto it = object.find("languages"); it != object.end()) {
            auto list = manifest::readStringList(it->second, "languages");
            if (!list) {
                return list.takeError();
            }
            result->languages = list.take();
        }
        if (const auto it = object.find("supportsKnownNotes"); it != object.end()) {
            if (!it->second.isBool()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  "supportsKnownNotes must be a boolean");
            }
            result->supportsKnownNotes = it->second.toBool();
        }

        if (const auto it = object.find("knobs"); it != object.end()) {
            if (!it->second.isObject()) {
                return srt::Error(srt::Error::InvalidFormat, "knobs must be an object");
            }
            const auto knobs = it->second.toObject();
            if (auto checked =
                    manifest::rejectUnknownKeys(knobs,
                                                {"boundaryThreshold", "boundaryRadius",
                                                 "noteThreshold", "notePresenceCutoff", "steps"},
                                                "the Note knobs");
                !checked) {
                return checked.takeError();
            }
            const std::pair<const char *, Common::L1::Knob NoteSchema::*> continuous[] = {
                {"boundaryThreshold",  &NoteSchema::boundaryThreshold },
                {"boundaryRadius",     &NoteSchema::boundaryRadius    },
                {"noteThreshold",      &NoteSchema::noteThreshold     },
                {"notePresenceCutoff", &NoteSchema::notePresenceCutoff},
            };
            for (const auto &[key, member] : continuous) {
                const auto knob = knobs.find(key);
                if (knob == knobs.end()) {
                    continue;
                }
                auto read = manifest::readKnob(knob->second, key);
                if (!read) {
                    return read.takeError();
                }
                result.get()->*member = read.take();
            }
            if (const auto knob = knobs.find("steps"); knob != knobs.end()) {
                auto read = manifest::readIntKnob(knob->second, "steps");
                if (!read) {
                    return read.takeError();
                }
                result->steps = read.take();
            }
        }
        return result;
    }

}
