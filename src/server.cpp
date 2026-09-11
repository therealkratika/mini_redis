#include "server.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

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

    constexpr char message[] = "Hello from MiniRedis!\r\n";
    std::size_t sent = 0;
    while (sent < sizeof(message) - 1) {
        const ssize_t result = send(client_fd, message + sent,
                                    sizeof(message) - 1 - sent, 0);
        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("send");
            close(client_fd);
            return false;
        }
        if (result == 0) {
            std::cerr << "send: connection closed before message was sent"
                      << std::endl;
            close(client_fd);
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }

    close(client_fd);
    return true;
}