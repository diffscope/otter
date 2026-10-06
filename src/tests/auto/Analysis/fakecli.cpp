// A stand-in for the engine command line tools the ggml providers drive, so the provider tests can
// run their whole path — spawn, protocol, parse, place — without any model. It answers the game
// `serve` protocol with a canned transcription and the tifa `align` command with a canned TextGrid;
// what it verifies is the plumbing, never the weights.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

namespace {

    /// The transcription the fake `serve` answers with: one voiced note and one rest, so the tests
    /// can watch the provider's cutoff drop the second.
    void serve() {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::printf("{\"type\":\"ready\"}\n");
        std::fflush(stdout);
        while (true) {
            char header[36];
            if (std::fread(header, 1, sizeof(header), stdin) != sizeof(header)) {
                break;
            }
            std::uint32_t magic = 0;
            std::uint32_t samples = 0;
            std::memcpy(&magic, header, sizeof(magic));
            if (magic == 0x54495155u) {
                break; // "UQIT": the quit signal of the protocol.
            }
            std::memcpy(&samples, header + 32, sizeof(samples));
            char discard[4096];
            std::uint32_t left = samples * sizeof(float);
            while (left > 0) {
                const std::size_t chunk = left < sizeof(discard) ? left : sizeof(discard);
                if (std::fread(discard, 1, chunk, stdin) != chunk) {
                    return;
                }
                left -= static_cast<std::uint32_t>(chunk);
            }
            std::printf("{\"type\":\"notes\",\"count\":2,\"notes\":["
                        "{\"o\":0.000000,\"d\":0.500000,\"p\":60.250000,\"v\":1},"
                        "{\"o\":0.500000,\"d\":1.500000,\"p\":0.300000,\"v\":0}]}\n");
            std::fflush(stdout);
        }
    }

    /// The alignment the fake `align` writes: two words of two phones each, tiling the span.
    void align(int argc, char **argv) {
        std::string outputDirectory;
        std::string stem = "span";
        // The audio is the first argument after the command; every other non-option argument is a
        // value of an option and says nothing about the output name.
        if (argc > 2) {
            const std::string argument = argv[2];
            const auto slash = argument.find_last_of("/\\");
            const auto dot = argument.rfind('.');
            std::string file =
                slash == std::string::npos ? argument : argument.substr(slash + 1);
            // A bare file name has no separator at all, and its dot still ends the stem: the
            // comparison against the separator only applies where there is one.
            if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
                file = file.substr(0, dot - (slash == std::string::npos ? 0 : slash + 1));
            }
            stem = file;
        }
        for (int i = 2; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "-o" && i + 1 < argc) {
                outputDirectory = argv[++i];
            }
        }
        if (outputDirectory.empty()) {
            outputDirectory = ".";
        }
        const std::string path = outputDirectory + "/" + stem + ".TextGrid";
        std::FILE *out = std::fopen(path.c_str(), "wb");
        if (out == nullptr) {
            std::fprintf(stderr, "fake cli: cannot write %s\n", path.c_str());
            return;
        }
        std::fprintf(out, R"(File type = "ooTextFile"
Object class = "TextGrid"

xmin = 0
xmax = 1
tiers? <exists>
size = 3
item []:
    item [1]:
        class = "IntervalTier"
        name = "texts"
        xmin = 0
        xmax = 1
        intervals: size = 3
        intervals [1]:
            xmin = 0
            xmax = 0.2
            text = ""
        intervals [2]:
            xmin = 0.2
            xmax = 0.6
            text = "ni"
        intervals [3]:
            xmin = 0.6
            xmax = 1
            text = "hao"
    item [2]:
        class = "IntervalTier"
        name = "words"
        xmin = 0
        xmax = 1
        intervals: size = 2
        intervals [1]:
            xmin = 0.2
            xmax = 0.6
            text = "ni"
        intervals [2]:
            xmin = 0.6
            xmax = 1
            text = "hao"
    item [3]:
        class = "IntervalTier"
        name = "phones"
        xmin = 0
        xmax = 1
        intervals: size = 4
        intervals [1]:
            xmin = 0.2
            xmax = 0.35
            text = "n"
        intervals [2]:
            xmin = 0.35
            xmax = 0.6
            text = "i"
        intervals [3]:
            xmin = 0.6
            xmax = 0.8
            text = "x"
        intervals [4]:
            xmin = 0.8
            xmax = 1
            text = "ao"
)");
        std::fclose(out);
    }

}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fakecli serve <model> | align <audio> [options]\n");
        return 2;
    }
    const std::string command = argv[1];
    if (command == "serve") {
        serve();
        return 0;
    }
    if (command == "align") {
        align(argc, argv);
        return 0;
    }
    std::fprintf(stderr, "fake cli: unknown command %s\n", command.c_str());
    return 2;
}
