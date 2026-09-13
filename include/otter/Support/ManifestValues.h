#ifndef OTTER_MANIFESTVALUES_H
#define OTTER_MANIFESTVALUES_H

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include <synthrt/Support/Expected.h>
#include <synthrt/Support/JSON.h>

#include <otter/Api/Common/1/CommonApiL1.h>
#include <otter/otter_global.h>

/// Readers shared by the analysis providers, so that two variants reading the same shape out of a
/// declaration cannot disagree about what it means or how a bad value is reported.
namespace otter::manifest {

    /// Reads a path, resolving a relative one against \a base.
    ///
    /// \a base is the directory of the declaration file the value came from, which is where spec
    /// 2.4 says a relative path resolves.
    OTTER_EXPORT srt::Expected<std::filesystem::path> readPath(const srt::JsonValue &value,
                                                               const std::filesystem::path &base,
                                                               std::string_view what);

    /// Reads an integer greater than zero.
    OTTER_EXPORT srt::Expected<int> readPositiveInt(const srt::JsonValue &value,
                                                    std::string_view what);

    /// Reads a number greater than zero.
    OTTER_EXPORT srt::Expected<double> readPositiveDouble(const srt::JsonValue &value,
                                                          std::string_view what);

    /// Reads a number in the inclusive range from 0 to 1.
    OTTER_EXPORT srt::Expected<double> readUnitDouble(const srt::JsonValue &value,
                                                      std::string_view what);

    /// Reads a non-empty string.
    OTTER_EXPORT srt::Expected<std::string> readString(const srt::JsonValue &value,
                                                       std::string_view what);

    /// Reads an array of non-empty strings that must not repeat.
    OTTER_EXPORT srt::Expected<std::vector<std::string>> readStringList(const srt::JsonValue &value,
                                                                        std::string_view what);

    /// Reads one continuous knob declaration: an object with \c minimum, \c maximum and
    /// \c default, the default lying inside the range. The knob comes back honored, since a
    /// module that does not honor a knob leaves it out rather than declaring it.
    OTTER_EXPORT srt::Expected<Api::Common::L1::Knob> readKnob(const srt::JsonValue &value,
                                                               std::string_view what);

    /// Reads one integral knob declaration, with the same shape as readKnob().
    OTTER_EXPORT srt::Expected<Api::Common::L1::IntKnob> readIntKnob(const srt::JsonValue &value,
                                                                     std::string_view what);

    /// Reads one boolean knob declaration: an object with \c default.
    OTTER_EXPORT srt::Expected<Api::Common::L1::FlagKnob> readFlagKnob(const srt::JsonValue &value,
                                                                       std::string_view what);

    /// Rejects an object carrying a key the contract does not define.
    ///
    /// These objects have a published schema, and a key outside it is a misspelling far more
    /// often than an extension: the declaration then reads as if it said something it did not,
    /// and the part it meant to say is silently absent.
    OTTER_EXPORT srt::Expected<void> rejectUnknownKeys(const srt::JsonObject &object,
                                                       std::initializer_list<const char *> allowed,
                                                       std::string_view what);

}

#endif // OTTER_MANIFESTVALUES_H
