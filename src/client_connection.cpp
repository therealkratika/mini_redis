#include "client_connection.h"

#include <cerrno>
#include <cstddef>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

ClientConnection::ClientConnection(int socket_fd) : fd(socket_fd) {}

ClientConnection::~ClientConnection() {
    close();
}

int ClientConnection::socket_fd() const {
    return fd;
}

bool ClientConnection::send(const std::string& data) {
    std::lock_guard<std::mutex> lock(send_mutex);
    if (closed) {
        return false;
    }

#ifdef MSG_NOSIGNAL
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
#endif

    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t result =
            ::send(fd, data.data() + sent, data.size() - sent, send_flags);
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

void ClientConnection::close() {
    std::lock_guard<std::mutex> lock(send_mutex);
    if (!closed) {
        ::close(fd);
        closed = true;
    }
}
