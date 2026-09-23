#include "replication.h"

#include "append_only_log.h"
#include "key_value_store.h"
#include "resp_parser.h"

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <netdb.h>
#include <system_error>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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

bool parse_integer(const std::string& text, std::int64_t& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return begin != end && result.ec == std::errc{} && result.ptr == end;
}

bool read_line(int socket_fd, std::string& line) {
    line.clear();
    char character = 0;
    while (line.size() < 256) {
        const ssize_t received = recv(socket_fd, &character, 1, 0);
        if (received == -1 && errno == EINTR) {
            continue;
        }
        if (received != 1) {
            return false;
        }
        line += character;
        if (line.size() >= 2 &&
            line.compare(line.size() - 2, 2, "\r\n") == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

Replication::Replication(std::shared_ptr<KeyValueStore> storage,
                         std::shared_ptr<AppendOnlyLog> persistence,
                         std::shared_ptr<std::mutex> mutation_mutex)
    : storage(std::move(storage)), persistence(std::move(persistence)),
      mutation_mutex(std::move(mutation_mutex)) {}

bool Replication::register_replica(
    const std::shared_ptr<ClientConnection>& client) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!client->send("+OK\r\n")) {
        return false;
    }
    if (replica_clients.insert(client.get()).second) {
        replicas.push_back(client);
    }
    return true;
}

void Replication::remove_replica(
    const std::shared_ptr<ClientConnection>& client) {
    std::lock_guard<std::mutex> lock(mutex);
    replica_clients.erase(client.get());
    replicas.erase(
        std::remove_if(replicas.begin(), replicas.end(),
                       [&client](const auto& replica) {
                           return replica.get() == client.get();
                       }),
        replicas.end());
}

bool Replication::is_replica_connection(
    const std::shared_ptr<ClientConnection>& client) {
    std::lock_guard<std::mutex> lock(mutex);
    return replica_clients.find(client.get()) != replica_clients.end();
}

void Replication::forward(const std::vector<std::string>& arguments) {
    std::vector<std::shared_ptr<ClientConnection>> current_replicas;
    {
        std::lock_guard<std::mutex> lock(mutex);
        current_replicas = replicas;
    }

    const std::string command = encode_command(arguments);
    for (const auto& replica : current_replicas) {
        if (!replica->send(command)) {
            remove_replica(replica);
        }
    }
}

bool Replication::become_replica(const std::string& host,
                                const std::string& port,
                                std::string& error) {
    std::lock_guard<std::mutex> setup_lock(replica_setup_mutex);
    if (replica_mode.load()) {
        error = "already connected to a primary";
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int lookup = getaddrinfo(host.c_str(), port.c_str(), &hints,
                                   &addresses);
    if (lookup != 0) {
        error = std::string("cannot resolve primary: ") +
                gai_strerror(lookup);
        return false;
    }

    int socket_fd = -1;
    for (addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype,
                           address->ai_protocol);
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
        error = "cannot connect to primary";
        return false;
    }

#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
    int no_sigpipe = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE,
                   &no_sigpipe, sizeof(no_sigpipe)) == -1) {
        error = "cannot configure primary connection";
        close(socket_fd);
        return false;
    }
#endif

    std::string greeting;
    auto upstream = std::make_shared<ClientConnection>(socket_fd);
    if (!read_line(socket_fd, greeting) ||
        greeting != "Hello from MiniRedis!\r\n" ||
        !upstream->send(encode_command({"REPLICA"}))) {
        error = "invalid primary greeting or handshake failed";
        upstream->close();
        return false;
    }

    std::string acknowledgement;
    if (!read_line(socket_fd, acknowledgement) || acknowledgement != "+OK\r\n") {
        error = "primary rejected replication handshake";
        upstream->close();
        return false;
    }

    replica_mode.store(true);
    try {
        std::thread(&Replication::receive_replication, this, upstream).detach();
    } catch (const std::system_error& exception) {
        replica_mode.store(false);
        error = std::string("cannot start replication thread: ") +
                exception.what();
        upstream->close();
        return false;
    }
    return true;
}

bool Replication::is_replica() const {
    return replica_mode.load();
}

bool Replication::apply_replicated(
    const std::vector<std::string>& arguments) {
    if (arguments.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(*mutation_mutex);
    if (arguments[0] == "SET" && arguments.size() == 3) {
        if (!persistence->append(arguments)) {
            return false;
        }
        storage->set(arguments[1], arguments[2]);
        return true;
    }
    if (arguments[0] == "DEL" && arguments.size() >= 2) {
        if (!persistence->append(arguments)) {
            return false;
        }
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            storage->del(arguments[i]);
        }
        return true;
    }
    if (arguments[0] == "EXPIREAT" && arguments.size() == 3) {
        std::int64_t timestamp_ms = 0;
        if (!parse_integer(arguments[2], timestamp_ms) ||
            !persistence->append(arguments)) {
            return false;
        }
        storage->expire_at(
            arguments[1],
            std::chrono::system_clock::time_point(
                std::chrono::milliseconds(timestamp_ms)));
        return true;
    }
    return false;
}

void Replication::receive_replication(
    const std::shared_ptr<ClientConnection>& upstream) {
    char buffer[1024];
    std::string pending;
    while (true) {
        const ssize_t received =
            recv(upstream->socket_fd(), buffer, sizeof(buffer), 0);
        if (received == -1 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            if (received == -1) {
                perror("replication recv");
            } else {
                std::cerr << "Primary disconnected" << std::endl;
            }
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
            if (result == RespParseResult::Invalid || consumed == 0 ||
                !apply_replicated(arguments)) {
                std::cerr << "Invalid command received from primary"
                          << std::endl;
                pending.clear();
                break;
            }
            pending.erase(0, consumed);
        }
        if (pending.empty() && received > 0) {
            continue;
        }
    }

    replica_mode.store(false);
    upstream->close();
}
