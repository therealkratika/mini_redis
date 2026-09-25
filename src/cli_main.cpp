#include <cerrno>
#include <cctype>
#include <cstddef>
#include <exception>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <netdb.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::string encode_command(const std::vector<std::string>& arguments) {
    std::string encoded = "*" + std::to_string(arguments.size()) + "\r\n";
    for (const std::string& argument : arguments) {
        encoded += "$" + std::to_string(argument.size()) + "\r\n";
        encoded += argument;
        encoded += "\r\n";
    }
    return encoded;
}

bool send_all(int socket_fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const ssize_t result =
            send(socket_fd, data.data() + sent, data.size() - sent,
                 send_flags);
        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("send");
            return false;
        }
        if (result == 0) {
            std::cerr << "Connection closed while sending command"
                      << std::endl;
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool read_exact(int socket_fd, std::string& data, std::size_t length) {
    while (data.size() < length) {
        char buffer[1024];
        const std::size_t needed = length - data.size();
        const ssize_t received =
            recv(socket_fd, buffer, needed < sizeof(buffer) ? needed
                                                            : sizeof(buffer),
                 0);
        if (received == -1 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return false;
        }
        data.append(buffer, static_cast<std::size_t>(received));
    }
    return true;
}

bool read_line(int socket_fd, std::string& line, std::string& raw) {
    line.clear();
    raw.clear();
    char character = 0;
    while (true) {
        if (!read_exact(socket_fd, raw, raw.size() + 1)) {
            return false;
        }
        character = raw.back();
        if (character == '\n') {
            if (raw.size() < 2 || raw[raw.size() - 2] != '\r') {
                return false;
            }
            line.assign(raw.data(), raw.size() - 2);
            return true;
        }
        if (raw.size() > 1024 * 1024) {
            return false;
        }
    }
}

bool read_frame(int socket_fd, std::string& frame) {
    std::string line;
    std::string raw;
    if (!read_line(socket_fd, line, raw) || line.empty()) {
        return false;
    }
    frame = raw;
    if (line[0] == '+' || line[0] == '-' || line[0] == ':') {
        return true;
    }
    if (line[0] == '$') {
        std::size_t length = 0;
        try {
            const long long parsed = std::stoll(line.substr(1));
            if (parsed < 0) {
                return parsed == -1;
            }
            length = static_cast<std::size_t>(parsed);
        } catch (const std::exception&) {
            return false;
        }
        std::string body;
        if (!read_exact(socket_fd, body, length + 2) ||
            body.compare(length, 2, "\r\n") != 0) {
            return false;
        }
        frame += body;
        return true;
    }
    if (line[0] == '*') {
        std::size_t count = 0;
        try {
            const long long parsed = std::stoll(line.substr(1));
            if (parsed < 0) {
                return true;
            }
            count = static_cast<std::size_t>(parsed);
        } catch (const std::exception&) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            std::string element;
            if (!read_frame(socket_fd, element)) {
                return false;
            }
            frame += element;
        }
        return true;
    }
    return false;
}

bool tokenize_command(const std::string& line,
                      std::vector<std::string>& arguments) {
    std::istringstream input(line);
    std::string command;
    if (!(input >> command)) {
        return true;
    }
    for (char& character : command) {
        character = static_cast<char>(
            std::toupper(static_cast<unsigned char>(character)));
    }

    arguments.push_back(command);
    if (command == "PUBLISH") {
        std::string channel;
        if (!(input >> channel)) {
            return true;
        }
        std::string message;
        std::getline(input, message);
        const std::size_t message_start = message.find_first_not_of(" \t");
        arguments.push_back(channel);
        arguments.push_back(message_start == std::string::npos
                                ? std::string{}
                                : message.substr(message_start));
        return true;
    }
    std::string word;
    while (input >> word) {
        arguments.push_back(word);
    }
    return true;
}

int connect_to_server() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int lookup =
        getaddrinfo("localhost", "6380", &hints, &addresses);
    if (lookup != 0) {
        std::cerr << "Unable to resolve localhost: "
                  << gai_strerror(lookup) << std::endl;
        return -1;
    }

    int socket_fd = -1;
    for (addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
        socket_fd =
            socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd == -1) {
            continue;
        }
        if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    if (socket_fd == -1) {
        perror("connect");
    }
    return socket_fd;
}

}  // namespace

int main() {
    const int socket_fd = connect_to_server();
    if (socket_fd == -1) {
        return 1;
    }

    std::string greeting;
    std::string raw_greeting;
    if (!read_line(socket_fd, greeting, raw_greeting)) {
        std::cerr << "Failed to read server greeting" << std::endl;
        close(socket_fd);
        return 1;
    }
    std::cout << greeting << std::endl;

    std::mutex output_mutex;
    std::thread reader([&] {
        std::string frame;
        while (read_frame(socket_fd, frame)) {
            std::lock_guard<std::mutex> lock(output_mutex);
            std::cout << frame << std::flush;
        }
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cerr << "Server connection closed" << std::endl;
    });

    std::string line;
    while (std::cout << "mini-redis> " && std::getline(std::cin, line)) {
        std::vector<std::string> arguments;
        tokenize_command(line, arguments);
        if (arguments.empty()) {
            continue;
        }
        if (arguments[0] == "QUIT" || arguments[0] == "EXIT") {
            break;
        }
        if (!send_all(socket_fd, encode_command(arguments))) {
            break;
        }
    }

    shutdown(socket_fd, SHUT_WR);
    reader.join();
    close(socket_fd);
    return 0;
}
