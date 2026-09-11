#include <otter/Support/ManifestValues.h>

#include <algorithm>
#include <set>

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
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " must not be empty");
        }
        auto path = stdc::path::from_utf8(text);
        if (path.is_absolute()) {
            return path;
        }
        return (base / path).lexically_normal();
    }

    srt::Expected<int> readPositiveInt(const srt::JsonValue &value, std::string_view what) {
        if (!value.isInt()) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " must be an integer");
        }
        const auto number = value.toInt();
        if (number < 1) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " must be greater than zero");
        }
        // Everything this reads is a rate, a count or a step budget, none of which is meaningfully
        // larger than an int, and narrowing without a check would turn a typo into a negative.
        if (number > 0x7fffffff) {
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " is out of range");
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
            return srt::Error(srt::Error::InvalidFormat,
                              std::string(what) + " must not be empty");
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

    srt::Expected<void> rejectUnknownKeys(const srt::JsonObject &object,
                                          std::initializer_list<const char *> allowed,
                                          std::string_view what) {
        for (const auto &[key, item] : object) {
            const bool known = std::any_of(allowed.begin(), allowed.end(), [&key](const char *one) {
                return key == one;
            });
            if (!known) {
                return srt::Error(srt::Error::InvalidFormat,
                                  std::string(what) + " carries an unknown key: " + key);
            }
        }
        return srt::Expected<void>();
    }

}
