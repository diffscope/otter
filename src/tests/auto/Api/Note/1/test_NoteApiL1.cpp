#include <utility>

#include <otter/Api/Note/1/NoteApiL1.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace Api = otter::Api::Note::L1;

BOOST_AUTO_TEST_CASE(variant_identity_and_task_identity_are_separate) {
    Api::NoteSchema schema("test.variant");
    Api::NoteImportOptions options("test.variant");
    Api::NoteRuntimeOptions runtime("test.variant");
    const srt::ContribSpecPayload *specPayloads[] = {&schema, &options, &runtime};
    for (const auto payload : specPayloads) {
        BOOST_CHECK(payload->triplet() ==
                    srt::ContribTriplet(Api::API_INTERFACE, Api::API_LEVEL, "test.variant"));
    }

    Api::NoteStartInput input;
    Api::NoteResult result;
    BOOST_CHECK_EQUAL(input.type(), Api::API_INTERFACE);
    BOOST_CHECK_EQUAL(input.version(), Api::API_LEVEL);
    BOOST_CHECK_EQUAL(result.type(), Api::API_INTERFACE);
    BOOST_CHECK_EQUAL(result.version(), Api::API_LEVEL);
    auto moved = std::move(input);
    BOOST_CHECK_EQUAL(moved.type(), Api::API_INTERFACE);
}
