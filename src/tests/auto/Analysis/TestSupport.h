#ifndef OTTER_TEST_SUPPORT_H
#define OTTER_TEST_SUPPORT_H

#include <string>

namespace otter::test {

    /// Returns why \a expected failed, or an empty string when it did not.
    ///
    /// Never call error() on an Expected that holds a value: it reads the other member of a union
    /// and toString() then walks a cause chain that is not there. Boost.Test evaluates the message
    /// argument of BOOST_REQUIRE_MESSAGE whether or not the assertion holds, so the obvious
    /// `BOOST_REQUIRE_MESSAGE(exp, exp.error().toString())` is that exact mistake on every passing
    /// assertion.
    template <class Expected>
    std::string why(const Expected &expected) {
        return expected ? std::string() : expected.error().toString();
    }

}

#endif // OTTER_TEST_SUPPORT_H
