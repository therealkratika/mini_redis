#include "server.h"
#include "append_only_log.h"
#include "client_connection.h"
#include "command_handler.h"
#include "pub_sub.h"
#include "resp_parser.h"
#include "replication.h"

#include <cerrno>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace {

void handle_client(const std::shared_ptr<ClientConnection>& client,
                   const std::shared_ptr<KeyValueStore>& storage,
                   const std::shared_ptr<AppendOnlyLog>& persistence,
                   const std::shared_ptr<std::mutex>& mutation_mutex,
                   const std::shared_ptr<PubSub>& pub_sub,
                   const std::shared_ptr<Replication>& replication) {
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
    int no_sigpipe = 1;
    if (setsockopt(client->socket_fd(), SOL_SOCKET, SO_NOSIGPIPE,
                   &no_sigpipe, sizeof(no_sigpipe)) == -1) {
        perror("setsockopt");
        pub_sub->remove_client(client);
        replication->remove_replica(client);
        client->close();
        return;
    }
#endif

    constexpr char greeting[] = "Hello from MiniRedis!\r\n";
    if (!client->send(greeting)) {
        pub_sub->remove_client(client);
        replication->remove_replica(client);
        client->close();
        return;
    }

    char buffer[1024];
    std::string pending;
    CommandHandler command_handler(*storage, *persistence, *mutation_mutex,
                                   *pub_sub, *replication, client);
    while (true) {
        const ssize_t received =
            recv(client->socket_fd(), buffer, sizeof(buffer), 0);
        if (received == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            break;
        }
        if (received == 0) {
            std::cout << "Client disconnected" << std::endl;
            break;
        }

        pending.append(buffer, static_cast<std::size_t>(received));
        bool close_connection = false;
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
                client->send(error);
                close_connection = true;
                break;
            }

            std::cout << "Received command:";
            for (const std::string& argument : arguments) {
                std::cout << ' ' << argument;
            }
            std::cout << std::endl;

            pending.erase(0, consumed);
            const std::string response = command_handler.handle(arguments);
            if (!client->send(response)) {
                close_connection = true;
                break;
            }
        }
        if (close_connection) {
            break;
        }
    }

    pub_sub->remove_client(client);
    replication->remove_replica(client);
    client->close();
}

}  // namespace

Server::Server(int port)
    : port(port), storage(std::make_shared<KeyValueStore>()),
      persistence(std::make_shared<AppendOnlyLog>()),
      mutation_mutex(std::make_shared<std::mutex>()),
      pub_sub(std::make_shared<PubSub>()),
      replication(std::make_shared<Replication>(
          storage, persistence, mutation_mutex)) {}

bool Server::start() {
    if (!persistence->replay(*storage)) {
        return false;
    }

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

    if (listen(server_fd, SOMAXCONN) == -1) {
        perror("listen");
        close(server_fd);
        return false;
    }

    std::cout << "MiniRedis listening on port " << port << std::endl;

    while (true) {
        int client_fd;
        do {
            client_fd = accept(server_fd, nullptr, nullptr);
        } while (client_fd == -1 && errno == EINTR);

        if (client_fd == -1) {
            perror("accept");
            close(server_fd);
            return false;
        }

        auto client = std::make_shared<ClientConnection>(client_fd);
        try {
            std::thread(handle_client, client, storage, persistence,
                        mutation_mutex, pub_sub, replication).detach();
        } catch (const std::system_error& error) {
            std::cerr << "Failed to start client thread: "
                      << error.what() << std::endl;
            client->close();
        }
    }
}