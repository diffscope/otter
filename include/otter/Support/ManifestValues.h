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

/// Readers shared by the analysis providers, so that two variants that read the same structure
/// from a declaration interpret it identically and report an invalid value identically.
///
/// Each reader returns the value read, or an \c InvalidFormat error whose message names the value
/// by \a what.
///
/// This header is not part of the public API. It is not installed, and its functions are compiled
/// into the private static library otter_support, which the library and each plugin in this tree
/// link individually. The readers hold no state, so a separate copy in each module behaves
/// identically to a shared copy. The readers may change together with the code in this tree
/// without notice.
namespace otter::manifest {

    /// Reads a path and resolves a relative path against \a base.
    ///
    /// \a base is the directory of the declaration file that contains the value; spec 2.4
    /// specifies that a relative path resolves against this directory. Backslashes are normalized
    /// to forward slashes before resolution.
    srt::Expected<std::filesystem::path> readPath(const srt::JsonValue &value,
                                                  const std::filesystem::path &base,
                                                  std::string_view what);

    /// Reads an integer greater than zero.
    srt::Expected<int> readPositiveInt(const srt::JsonValue &value, std::string_view what);

    /// Reads a number greater than zero.
    srt::Expected<double> readPositiveDouble(const srt::JsonValue &value, std::string_view what);

    /// Reads a number in the inclusive range from 0 to 1.
    srt::Expected<double> readUnitDouble(const srt::JsonValue &value, std::string_view what);

    /// Reads a non-empty string.
    srt::Expected<std::string> readString(const srt::JsonValue &value, std::string_view what);

    /// Reads an array of non-empty strings that must not repeat.
    srt::Expected<std::vector<std::string>> readStringList(const srt::JsonValue &value,
                                                           std::string_view what);

    /// Reads a language identifier: an ISO 639-3 code of three lowercase letters.
    ///
    /// The grammar is identical to the grammar that wolf applies to its linguists and singers, so
    /// that a host can pass an identifier obtained from a singer to an analyzer unchanged.
    srt::Expected<std::string> readLanguage(const srt::JsonValue &value, std::string_view what);

    /// Reads an array of distinct language identifiers, each validated as by readLanguage().
    srt::Expected<std::vector<std::string>> readLanguageList(const srt::JsonValue &value,
                                                             std::string_view what);

    /// Reads a scheme name: lowercase letters and digits in groups joined by single hyphens.
    ///
    /// A scheme identifies the notation of a language's pronunciation, such as pinyin or arpabet.
    /// The names and their grammar are wolf's, so that a host can pair an analyzer with a linguist
    /// by scheme name.
    srt::Expected<std::string> readScheme(const srt::JsonValue &value, std::string_view what);

    /// Reads one continuous knob declaration: an object with \c minimum, \c maximum and
    /// \c default, with \c default inside the range. The returned knob is marked as honored,
    /// because a module that does not honor a knob omits the knob from its declaration.
    srt::Expected<Api::Common::L1::Knob> readKnob(const srt::JsonValue &value,
                                                  std::string_view what);

    /// Reads one integral knob declaration, with the same shape as readKnob().
    srt::Expected<Api::Common::L1::IntKnob> readIntKnob(const srt::JsonValue &value,
                                                        std::string_view what);

    /// Reads one boolean knob declaration: an object with \c default.
    srt::Expected<Api::Common::L1::FlagKnob> readFlagKnob(const srt::JsonValue &value,
                                                          std::string_view what);

    /// Rejects an object that contains a key the contract does not define.
    ///
    /// These objects have a published schema, and a key outside the schema is far more often a
    /// misspelling than an extension. An accepted misspelling would leave the intended value
    /// silently absent from the declaration.
    ///
    /// \return An empty value if every key of \a object is in \a allowed, and an
    /// \c InvalidFormat error that names the first unknown key otherwise.
    srt::Expected<void> rejectUnknownKeys(const srt::JsonObject &object,
                                          std::initializer_list<const char *> allowed,
                                          std::string_view what);
}

#endif // OTTER_MANIFESTVALUES_H
