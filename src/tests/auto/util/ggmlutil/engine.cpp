#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc > 1 && std::strcmp(argv[1], "partial") == 0) {
        std::fputs("partial", stdout);
        return 0;
    }
    std::fputs("ready\n", stdout);
    std::fflush(stdout);
    char input[128];
    if (std::fgets(input, sizeof(input), stdin)) {
        std::fputs(input, stdout);
        std::fflush(stdout);
    }
    return 0;
}
