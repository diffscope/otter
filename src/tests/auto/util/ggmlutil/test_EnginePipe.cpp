#include <string>

#include <ggmlutil/EnginePipe.h>

#define BOOST_TEST_MAIN
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_CASE(short_lines_are_delivered_before_the_child_exits) {
    stdc::Popen process;
    otter::ggml::prepareEngine(process);
    process.args({OTTER_TEST_PIPE_ENGINE})
        .standardInput(stdc::Popen::Pipe)
        .standardOutput(stdc::Popen::Pipe);
    BOOST_REQUIRE(process.start());
    std::string line;
    BOOST_REQUIRE(otter::ggml::readLineFromEngine(process.standardOutput(), line));
    BOOST_CHECK_EQUAL(line, "ready");
    const std::string request = "request\n";
    BOOST_REQUIRE(
        otter::ggml::writeToEngine(process.standardInput(), request.data(), request.size()));
    BOOST_REQUIRE(otter::ggml::readLineFromEngine(process.standardOutput(), line));
    BOOST_CHECK_EQUAL(line, "request");
    BOOST_CHECK(!otter::ggml::readLineFromEngine(process.standardOutput(), line));
    BOOST_REQUIRE(process.wait(5000));
}

BOOST_AUTO_TEST_CASE(eof_without_newline_is_not_a_complete_response) {
    stdc::Popen process;
    otter::ggml::prepareEngine(process);
    process.args({OTTER_TEST_PIPE_ENGINE, "partial"}).standardOutput(stdc::Popen::Pipe);
    BOOST_REQUIRE(process.start());
    std::string line;
    BOOST_CHECK(!otter::ggml::readLineFromEngine(process.standardOutput(), line));
    BOOST_REQUIRE(process.wait(5000));
}
