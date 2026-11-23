#include <otter/Api/Common/1/CommonApiL1.h>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <synthrt/Support/Json.h>

#include <analysisutil/ManifestValues.h>

namespace otter::Api::Common::L1 {

    namespace {

        // Returns the object of a numeric knob declaration after checking that it holds exactly
        // the keys minimum, maximum and default. Both numeric knob readers use it.
        srt::Expected<srt::JsonObject> numericKnobObject(const srt::JsonValue &value,
                                                         std::string_view what) {
            if (!value.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  std::string(what) + " must be an object");
            }
            auto object = value.toObject();
            if (auto checked =
                    manifest::rejectUnknownKeys(object, {"minimum", "maximum", "default"}, what);
                !checked) {
                return checked.takeError();
            }
            for (const char *key : {"minimum", "maximum", "default"}) {
                if (object.find(key) == object.end()) {
                    return srt::Error(srt::Error::InvalidFormat,
                                      std::string(what) + " needs a " + key);
                }
            }
            return object;
        }

        // Checks that the declared range of a knob is ordered and holds its default. The two
        // numeric readers pass declarations of different types, and one template serves both.
        template <typename KnobType>
        srt::Expected<void> checkKnobRange(const KnobType &knob, std::string_view what) {
            if (knob.minimum > knob.maximum || knob.defaultValue < knob.minimum ||
                knob.defaultValue > knob.maximum) {
                return srt::Error(srt::Error::InvalidFormat,
                                  std::string(what) + " must have minimum <= default <= maximum");
            }
            return {};
        }

        // Resolves a numeric knob for both numeric choose() functions, which differ only in the
        // type of the value and of the declaration. The bounds keep the open endpoints of a
        // range: a value equal to minimum or maximum is accepted.
        template <typename Value, typename KnobType>
        srt::Expected<Value> chooseNumeric(const std::optional<Value> &given, const KnobType &knob,
                                           Value fallback, std::string_view what) {
            if (!knob.honored) {
                return fallback;
            }
            if (!given) {
                return knob.defaultValue;
            }
            if (*given < knob.minimum || *given > knob.maximum) {
                return srt::Error(
                    srt::Error::InvalidArgument,
                    std::string(what) + " must be between " + std::to_string(knob.minimum) +
                        " and " + std::to_string(knob.maximum));
            }
            return *given;
        }

    }

    srt::Expected<Knob> Knob::read(const srt::JsonValue &value, std::string_view what) {
        auto object = numericKnobObject(value, what);
        if (!object) {
            return object.takeError();
        }
        Knob knob;
        knob.honored = true;
        const std::pair<const char *, double *> fields[] = {
            {"minimum", &knob.minimum     },
            {"maximum", &knob.maximum     },
            {"default", &knob.defaultValue},
        };
        for (const auto &[key, target] : fields) {
            const auto &item = object->find(key)->second;
            if (!item.isNumber()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  std::string(what) + "." + key + " must be a number");
            }
            *target = item.toDouble();
        }
        if (auto checked = checkKnobRange(knob, what); !checked) {
            return checked.takeError();
        }
        return knob;
    }

    srt::Expected<IntKnob> IntKnob::read(const srt::JsonValue &value, std::string_view what) {
        auto object = numericKnobObject(value, what);
        if (!object) {
            return object.takeError();
        }
        IntKnob knob;
        knob.honored = true;
        const std::pair<const char *, int *> fields[] = {
            {"minimum", &knob.minimum     },
            {"maximum", &knob.maximum     },
            {"default", &knob.defaultValue},
        };
        for (const auto &[key, target] : fields) {
            const auto &item = object->find(key)->second;
            if (!item.isInt() || item.toInt() < -0x7fffffff || item.toInt() > 0x7fffffff) {
                return srt::Error(srt::Error::InvalidFormat,
                                  std::string(what) + "." + key + " must be an integer");
            }
            *target = static_cast<int>(item.toInt());
        }
        if (auto checked = checkKnobRange(knob, what); !checked) {
            return checked.takeError();
        }
        return knob;
    }

    srt::Expected<FlagKnob> FlagKnob::read(const srt::JsonValue &value, std::string_view what) {
        if (!value.isObject()) {
            return srt::Error(srt::Error::InvalidFormat, std::string(what) + " must be an object");
        }
        const auto object = value.toObject();
        if (auto checked = manifest::rejectUnknownKeys(object, {"default"}, what); !checked) {
            return checked.takeError();
        }
        const auto it = object.find("default");
        if (it == object.end() || !it->second.isBool()) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " needs a boolean default");
        }
        FlagKnob knob;
        knob.honored = true;
        knob.defaultValue = it->second.toBool();
        return knob;
    }

    srt::Expected<double> Knob::choose(const std::optional<double> &given, double fallback,
                                       std::string_view what) const {
        return chooseNumeric(given, *this, fallback, what);
    }

    srt::Expected<int> IntKnob::choose(const std::optional<int> &given, int fallback,
                                       std::string_view what) const {
        return chooseNumeric(given, *this, fallback, what);
    }

    bool FlagKnob::choose(const std::optional<bool> &given, bool fallback) const {
        if (!honored) {
            return fallback;
        }
        return given.value_or(defaultValue);
    }

}