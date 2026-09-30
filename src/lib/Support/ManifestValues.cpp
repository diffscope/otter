#include <otter/Support/ManifestValues.h>

#include <algorithm>
#include <set>
#include <utility>

#include <stdcorelib/path.h>

namespace otter::manifest {

    srt::Expected<std::filesystem::path> readPath(const srt::JsonValue &value,
                                                  const std::filesystem::path &base,
                                                  std::string_view what) {
        if (!value.isString()) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " must be a string holding a path");
        }
        const auto text = value.toString();
        if (text.empty()) {
            return srt::Error(srt::Error::InvalidFormat, std::string(what) + " must not be empty");
        }
        // Spec 2.4 counts both / and \ as separators and requires the reader to normalize the
        // path before the host file system receives it, so that one declaration names the same
        // file on a host on which only / is a separator. Windows already treats \ as a separator,
        // so the normalization makes the other platforms agree with Windows rather than serving
        // as a Windows-only convenience. synthrt's loader rewrites the separators in the same way
        // for the paths it resolves itself (resolvePath in SingerContrib).
        std::string normalized = text;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        auto path = stdc::path::from_utf8(normalized);
        if (path.is_absolute()) {
            return path;
        }
        return (base / path).lexically_normal();
    }

    srt::Expected<int> readPositiveInt(const srt::JsonValue &value, std::string_view what) {
        if (!value.isInt()) {
            return srt::Error(srt::Error::InvalidFormat, std::string(what) + " must be an integer");
        }
        const auto number = value.toInt();
        if (number < 1) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " must be greater than zero");
        }
        // Every value read by this function is a rate, a count or a step count, none of which
        // reasonably exceeds the range of int, and narrowing without a check would turn a typing
        // error into a negative number.
        if (number > 0x7fffffff) {
            return srt::Error(srt::Error::InvalidFormat, std::string(what) + " is out of range");
        }
        return static_cast<int>(number);
    }

    srt::Expected<double> readPositiveDouble(const srt::JsonValue &value, std::string_view what) {
        if (!value.isNumber()) {
            return srt::Error(srt::Error::InvalidFormat, std::string(what) + " must be a number");
        }
        const auto number = value.toDouble();
        if (!(number > 0)) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " must be greater than zero");
        }
        return number;
    }

    srt::Expected<double> readUnitDouble(const srt::JsonValue &value, std::string_view what) {
        if (!value.isNumber()) {
            return srt::Error(srt::Error::InvalidFormat, std::string(what) + " must be a number");
        }
        const auto number = value.toDouble();
        if (number < 0 || number > 1) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " must be between 0 and 1");
        }
        return number;
    }

    srt::Expected<std::string> readString(const srt::JsonValue &value, std::string_view what) {
        if (!value.isString()) {
            return srt::Error(srt::Error::InvalidFormat, std::string(what) + " must be a string");
        }
        auto text = value.toString();
        if (text.empty()) {
            return srt::Error(srt::Error::InvalidFormat, std::string(what) + " must not be empty");
        }
        return text;
    }

    srt::Expected<std::vector<std::string>> readStringList(const srt::JsonValue &value,
                                                           std::string_view what) {
        if (!value.isArray()) {
            return srt::Error(srt::Error::InvalidFormat, std::string(what) + " must be an array");
        }
        std::vector<std::string> result;
        std::set<std::string> seen;
        for (const auto &item : value.toArray()) {
            auto text = readString(item, what);
            if (!text) {
                return text.takeError();
            }
            auto entry = text.take();
            if (!seen.insert(entry).second) {
                return srt::Error(srt::Error::InvalidFormat,
                                  std::string(what) + " repeats " + entry);
            }
            result.push_back(std::move(entry));
        }
        return result;
    }

    namespace {

        // Checks the grammar of a language identifier for both the single-value and list readers.
        srt::Expected<void> checkLanguage(const std::string &code, std::string_view what) {
            if (code.size() != 3 || !std::all_of(code.begin(), code.end(), [](char c) {
                    return c >= 'a' && c <= 'z';
                })) {
                return srt::Error(srt::Error::InvalidFormat,
                                  std::string(what) +
                                      " must be an ISO 639-3 code of three lowercase letters, "
                                      "and " +
                                      code + " is not");
            }
            return {};
        }

    }

    srt::Expected<std::string> readLanguage(const srt::JsonValue &value, std::string_view what) {
        auto text = readString(value, what);
        if (!text) {
            return text.takeError();
        }
        if (auto checked = checkLanguage(*text, what); !checked) {
            return checked.takeError();
        }
        return text;
    }

    srt::Expected<std::vector<std::string>> readLanguageList(const srt::JsonValue &value,
                                                             std::string_view what) {
        auto list = readStringList(value, what);
        if (!list) {
            return list.takeError();
        }
        for (const auto &code : *list) {
            if (auto checked = checkLanguage(code, what); !checked) {
                return checked.takeError();
            }
        }
        return list;
    }

    srt::Expected<std::string> readScheme(const srt::JsonValue &value, std::string_view what) {
        auto text = readString(value, what);
        if (!text) {
            return text.takeError();
        }
        const auto &name = *text;
        bool valid = name.front() != '-' && name.back() != '-';
        for (std::size_t i = 0; valid && i < name.size(); ++i) {
            const char c = name[i];
            const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            valid = alnum || (c == '-' && name[i + 1] != '-');
        }
        if (!valid) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " must be lowercase letters and digits in "
                                                  "groups joined by single hyphens, and " +
                                  name + " is not");
        }
        return text;
    }

    namespace {

        /// Returns the object of a numeric knob declaration after checking that it contains
        /// exactly the keys minimum, maximum and default. Both numeric knob readers use it.
        srt::Expected<srt::JsonObject> knobObject(const srt::JsonValue &value,
                                                  std::string_view what) {
            if (!value.isObject()) {
                return srt::Error(srt::Error::InvalidFormat,
                                  std::string(what) + " must be an object");
            }
            auto object = value.toObject();
            if (auto checked = rejectUnknownKeys(object, {"minimum", "maximum", "default"}, what);
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

    }

    srt::Expected<Api::Common::L1::Knob> readKnob(const srt::JsonValue &value,
                                                  std::string_view what) {
        auto object = knobObject(value, what);
        if (!object) {
            return object.takeError();
        }
        Api::Common::L1::Knob knob;
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
        if (knob.minimum > knob.maximum || knob.defaultValue < knob.minimum ||
            knob.defaultValue > knob.maximum) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " must have minimum <= default <= maximum");
        }
        return knob;
    }

    srt::Expected<Api::Common::L1::IntKnob> readIntKnob(const srt::JsonValue &value,
                                                        std::string_view what) {
        auto object = knobObject(value, what);
        if (!object) {
            return object.takeError();
        }
        Api::Common::L1::IntKnob knob;
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
        if (knob.minimum > knob.maximum || knob.defaultValue < knob.minimum ||
            knob.defaultValue > knob.maximum) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " must have minimum <= default <= maximum");
        }
        return knob;
    }

    srt::Expected<Api::Common::L1::FlagKnob> readFlagKnob(const srt::JsonValue &value,
                                                          std::string_view what) {
        if (!value.isObject()) {
            return srt::Error(srt::Error::InvalidFormat, std::string(what) + " must be an object");
        }
        const auto object = value.toObject();
        if (auto checked = rejectUnknownKeys(object, {"default"}, what); !checked) {
            return checked.takeError();
        }
        const auto it = object.find("default");
        if (it == object.end() || !it->second.isBool()) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " needs a boolean default");
        }
        Api::Common::L1::FlagKnob knob;
        knob.honored = true;
        knob.defaultValue = it->second.toBool();
        return knob;
    }

    srt::Expected<void> rejectUnknownKeys(const srt::JsonObject &object,
                                          std::initializer_list<const char *> allowed,
                                          std::string_view what) {
        for (const auto &[key, item] : object) {
            const bool known = std::any_of(allowed.begin(), allowed.end(),
                                           [&key](const char *one) { return key == one; });
            if (!known) {
                return srt::Error(srt::Error::InvalidFormat,
                                  std::string(what) + " carries an unknown key: " + key);
            }
        }
        return srt::Expected<void>();
    }

}
