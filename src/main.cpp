#include "server.h"

#include <charconv>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    int port = 6380;
    if (argc > 2) {
        std::cerr << "Usage: miniredis [port]" << std::endl;
        return 2;
    }
    if (argc == 2) {
        const std::string port_argument(argv[1]);
        const char* begin = port_argument.data();
        const char* end = begin + port_argument.size();
        const auto result = std::from_chars(begin, end, port);
        if (result.ec != std::errc{} || result.ptr != end ||
            port < 1 || port > 65535) {
            std::cerr << "Port must be an integer between 1 and 65535"
                      << std::endl;
            return 2;
        }
    }

    Server server(port);
    return server.start() ? 0 : 1;
}