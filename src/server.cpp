#include "server.h"
#include "resp_parser.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace {

bool send_all(int socket_fd, const char* data, std::size_t length) {
#ifdef MSG_NOSIGNAL
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
#endif

    std::size_t sent = 0;
    while (sent < length) {
        const ssize_t result = send(socket_fd, data + sent, length - sent,
                                    send_flags);
        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("send");
            return false;
        }
        if (result == 0) {
            std::cerr << "send: connection closed before response was sent"
                      << std::endl;
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

}  // namespace

Server::Server(int port) : port(port) {}

bool Server::start() {
    const int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return false;
    }

    int reuse_address = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) == -1) {
        perror("setsockopt");
        close(server_fd);
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(port));

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) == -1) {
        perror("bind");
        close(server_fd);
        return false;
    }

    if (listen(server_fd, 1) == -1) {
        perror("listen");
        close(server_fd);
        return false;
    }

    std::cout << "MiniRedis listening on port " << port << std::endl;

    int client_fd;
    do {
        client_fd = accept(server_fd, nullptr, nullptr);
    } while (client_fd == -1 && errno == EINTR);

    if (client_fd == -1) {
        perror("accept");
        close(server_fd);
        return false;
    }
    close(server_fd);

#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
    int no_sigpipe = 1;
    if (setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE,
                   &no_sigpipe, sizeof(no_sigpipe)) == -1) {
        perror("setsockopt");
        close(client_fd);
        return false;
    }
#endif

    constexpr char greeting[] = "Hello from MiniRedis!\r\n";
    if (!send_all(client_fd, greeting, sizeof(greeting) - 1)) {
        close(client_fd);
        return false;
    }

    constexpr char response[] = "+OK\r\n";
    char buffer[1024];
    std::string pending;
    bool success = true;
    while (true) {
        const ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
        if (received == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            success = false;
            break;
        }
        if (received == 0) {
            std::cout << "Client disconnected" << std::endl;
            break;
        }

        pending.append(buffer, static_cast<std::size_t>(received));
        while (!pending.empty()) {
            std::vector<std::string> arguments;
            std::size_t consumed = 0;
            const RespParseResult result =
                parse_resp_command(pending, arguments, consumed);
            if (result == RespParseResult::Incomplete) {
                break;
            }
            if (result == RespParseResult::Invalid) {
                constexpr char error[] = "-ERR invalid RESP request\r\n";
                send_all(client_fd, error, sizeof(error) - 1);
                success = false;
                break;
            }

            std::cout << "Received command:";
            for (const std::string& argument : arguments) {
                std::cout << ' ' << argument;
            }
            std::cout << std::endl;

            pending.erase(0, consumed);
            if (!send_all(client_fd, response, sizeof(response) - 1)) {
                success = false;
                break;
            }
        }
        if (!success) {
            break;
        }
    }

    close(client_fd);
    return success;
}