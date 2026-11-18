#include <utility>

#include <otter/Api/Note/1/NoteApiL1.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

namespace Api = otter::Api::Note::L1;

BOOST_AUTO_TEST_CASE(contract_identity_and_task_identity_are_preserved) {
    Api::Exports schema{};
    Api::ImportOptions options{};
    Api::RuntimeOptions runtime{};
    const srt::ContribSpecPayload *specPayloads[] = {&schema, &options, &runtime};
    for (const auto payload : specPayloads) {
        BOOST_CHECK(payload->contract() == Api::Contract);
    }

    Api::StartInput input;
    Api::Result result;
    BOOST_CHECK_EQUAL(input.type(), Api::Contract.interfaceId());
    BOOST_CHECK_EQUAL(input.version(), Api::Contract.level());
    BOOST_CHECK_EQUAL(result.type(), Api::Contract.interfaceId());
    BOOST_CHECK_EQUAL(result.version(), Api::Contract.level());
    auto moved = std::move(input);
    BOOST_CHECK_EQUAL(moved.type(), Api::Contract.interfaceId());
}
