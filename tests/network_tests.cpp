#include "server.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

std::string encode(const std::vector<std::string>& arguments) {
    std::string request = "*" + std::to_string(arguments.size()) + "\r\n";
    for (const auto& argument : arguments) {
        request += "$" + std::to_string(argument.size()) + "\r\n";
        request += argument + "\r\n";
    }
    return request;
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t result =
            send(fd, data.data() + sent, data.size() - sent, 0);
        if (result == -1 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool read_exact(int fd, std::string& data, std::size_t target) {
    while (data.size() < target) {
        char buffer[1024];
        const ssize_t received = recv(fd, buffer, target - data.size(), 0);
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

bool read_line(int fd, std::string& line, std::string& raw) {
    raw.clear();
    char character = 0;
    while (raw.size() < 1024 * 1024) {
        if (!read_exact(fd, raw, raw.size() + 1)) {
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
    }
    return false;
}

bool read_frame(int fd, std::string& frame) {
    std::string line;
    std::string raw;
    if (!read_line(fd, line, raw) || line.empty()) {
        return false;
    }
    frame = raw;
    if (line[0] == '+' || line[0] == '-' || line[0] == ':') {
        return true;
    }
    if (line[0] == '$') {
        long long parsed = -1;
        try {
            parsed = std::stoll(line.substr(1));
        } catch (...) {
            return false;
        }
        if (parsed == -1) {
            return true;
        }
        if (parsed < 0) {
            return false;
        }
        std::string body;
        if (!read_exact(fd, body, static_cast<std::size_t>(parsed) + 2) ||
            body.compare(static_cast<std::size_t>(parsed), 2, "\r\n") != 0) {
            return false;
        }
        frame += body;
        return true;
    }
    if (line[0] == '*') {
        long long count = -1;
        try {
            count = std::stoll(line.substr(1));
        } catch (...) {
            return false;
        }
        if (count < 0) {
            return true;
        }
        for (long long i = 0; i < count; ++i) {
            std::string element;
            if (!read_frame(fd, element)) {
                return false;
            }
            frame += element;
        }
        return true;
    }
    return false;
}

int free_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
        -1) {
        close(fd);
        return -1;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) ==
        -1) {
        close(fd);
        return -1;
    }
    const int port = ntohs(address.sin_port);
    close(fd);
    return port;
}

struct ChildServer {
    pid_t pid = -1;

    ChildServer() = default;
    ChildServer(const ChildServer&) = delete;
    ChildServer& operator=(const ChildServer&) = delete;

    ChildServer(ChildServer&& other) noexcept : pid(other.pid) {
        other.pid = -1;
    }

    ChildServer& operator=(ChildServer&& other) noexcept {
        if (this != &other) {
            if (pid > 0) {
                kill(pid, SIGTERM);
                waitpid(pid, nullptr, 0);
            }
            pid = other.pid;
            other.pid = -1;
        }
        return *this;
    }

    ~ChildServer() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    }
};

ChildServer start_server(int port, const std::filesystem::path& directory) {
    ChildServer child;
    child.pid = fork();
    if (child.pid == 0) {
        if (chdir(directory.c_str()) == -1) {
            _exit(2);
        }
        const int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd != -1) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        Server server(port);
        _exit(server.start() ? 0 : 1);
    }
    return child;
}

int connect_client(int port) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == -1) {
            return -1;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (connect(fd, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0) {
            std::string greeting;
            std::string raw;
            if (read_line(fd, greeting, raw) &&
                greeting == "Hello from MiniRedis!") {
                return fd;
            }
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -1;
}

std::string command(int fd, const std::vector<std::string>& arguments) {
    if (!send_all(fd, encode(arguments))) {
        return {};
    }
    std::string response;
    return read_frame(fd, response) ? response : std::string{};
}

void test_tcp_commands(int port) {
    const int client = connect_client(port);
    check(client != -1, "connect to server");
    check(command(client, {"SET", "name", "Kratika"}) == "+OK\r\n",
          "TCP SET response");
    check(command(client, {"GET", "name"}) == "$7\r\nKratika\r\n",
          "TCP GET response");
    check(command(client, {"DEL", "name"}) == ":1\r\n",
          "TCP DEL response");
    check(command(client, {"GET", "name"}) == "$-1\r\n",
          "TCP GET after DEL");
    check(command(client, {"TTL", "name"}) == ":-2\r\n",
          "TCP TTL missing key");
    check(command(client, {"SET", "ttl-key", "value"}) == "+OK\r\n",
          "TCP SET for TTL");
    check(command(client, {"EXPIRE", "ttl-key", "1"}) == ":1\r\n",
          "TCP EXPIRE");
    check(command(client, {"TTL", "ttl-key"}) == ":1\r\n" ||
              command(client, {"TTL", "ttl-key"}) == ":0\r\n",
          "TCP TTL for expiring key");
    check(command(client, {"NO-SUCH-COMMAND"}) ==
              "-ERR unknown command\r\n",
          "TCP unknown command error");
    check(command(client, {"SET", "missing-value"}) ==
              "-ERR wrong number of arguments for 'set' command\r\n",
          "TCP incorrect argument count error");
    check(command(client, {"GET"}) ==
              "-ERR wrong number of arguments for 'get' command\r\n",
          "TCP GET argument count error");
    check(command(client, {"EXPIRE", "ttl-key", "not-a-number"}) ==
              "-ERR value is not an integer or out of range\r\n",
          "TCP invalid TTL error");
    close(client);

    const int malformed = connect_client(port);
    check(malformed != -1, "connect malformed-request client");
    check(send_all(malformed, "*x\r\n"), "send malformed RESP request");
    std::string malformed_response;
    check(read_frame(malformed, malformed_response) &&
              malformed_response ==
                  "-ERR Protocol error: invalid RESP request\r\n",
          "malformed RESP gets protocol error response");
    close(malformed);

    const int truncated = connect_client(port);
    check(truncated != -1, "connect truncated-request client");
    check(send_all(truncated, "*2\r\n$3\r\nGET\r\n$"), "send partial RESP");
    shutdown(truncated, SHUT_WR);
    std::string truncated_response;
    check(read_frame(truncated, truncated_response) &&
              truncated_response ==
                  "-ERR Protocol error: incomplete RESP request\r\n",
          "truncated RESP gets protocol error response");
    close(truncated);

    const int disconnected = connect_client(port);
    check(disconnected != -1, "connect client that disconnects early");
    close(disconnected);
    const int after_disconnect = connect_client(port);
    check(after_disconnect != -1 &&
              command(after_disconnect, {"PING"}) == "+PONG\r\n",
          "server remains healthy after client disconnect");
    close(after_disconnect);
}

void test_persistence_error(int port,
                            const std::filesystem::path& directory) {
    const auto log_path = directory / "appendonly.aof";
    const auto backup_path = directory / "appendonly.saved";
    if (std::filesystem::exists(log_path)) {
        std::filesystem::rename(log_path, backup_path);
    }
    std::filesystem::create_directory(log_path);

    const int client = connect_client(port);
    check(client != -1, "connect for persistence error test");
    check(command(client, {"SET", "must-not-write", "value"}) ==
              "-ERR persistence failure\r\n",
          "TCP persistence failure returns Redis error");
    check(command(client, {"GET", "must-not-write"}) == "$-1\r\n",
          "failed persistent mutation is not applied");
    check(command(client, {"PING"}) == "+PONG\r\n",
          "server survives persistence error");
    close(client);
}

void test_pub_sub(int port) {
    const int subscriber_one = connect_client(port);
    const int subscriber_two = connect_client(port);
    check(subscriber_one != -1 && subscriber_two != -1,
          "connect Pub/Sub clients");
    check(command(subscriber_one, {"SUBSCRIBE", "events"}) ==
              "*3\r\n$9\r\nsubscribe\r\n$6\r\nevents\r\n:1\r\n",
          "SUBSCRIBE acknowledgement");
    check(command(subscriber_two, {"SUBSCRIBE", "events"}) ==
              "*3\r\n$9\r\nsubscribe\r\n$6\r\nevents\r\n:1\r\n",
          "second SUBSCRIBE acknowledgement");

    const int publisher = connect_client(port);
    check(publisher != -1, "connect Pub/Sub publisher");
    check(command(publisher, {"PUBLISH", "events", "hello clients"}) ==
              ":2\r\n",
          "PUBLISH subscriber count");
    const std::string expected =
        "*3\r\n$7\r\nmessage\r\n$6\r\nevents\r\n$13\r\nhello clients\r\n";
    std::string message;
    check(read_frame(subscriber_one, message) && message == expected,
          "first subscriber receives message frame");
    check(read_frame(subscriber_two, message) && message == expected,
          "second subscriber receives message frame");
    close(publisher);
    close(subscriber_one);
    close(subscriber_two);
}

void test_replication(int primary_port, int replica_port) {
    const int replica_admin = connect_client(replica_port);
    check(replica_admin != -1, "connect replica");
    check(command(replica_admin,
                  {"REPLICAOF", "127.0.0.1",
                   std::to_string(primary_port)}) == "+OK\r\n",
          "configure replica with REPLICAOF");

    const int primary = connect_client(primary_port);
    check(primary != -1, "connect primary");
    check(command(primary, {"SET", "replica-key", "replicated"}) == "+OK\r\n",
          "primary SET");
    check(command(primary, {"SET", "delete-key", "remove"}) == "+OK\r\n",
          "primary SET before DEL");
    check(command(primary, {"SET", "expiring-key", "short-lived"}) ==
              "+OK\r\n",
          "primary SET before EXPIRE");
    check(command(primary, {"EXPIRE", "expiring-key", "1"}) == ":1\r\n",
          "primary EXPIRE");
    check(command(primary, {"DEL", "delete-key"}) == ":1\r\n",
          "primary DEL");

    const int replica_reader = connect_client(replica_port);
    check(replica_reader != -1, "connect replica reader");
    bool replicated = false;
    for (int attempt = 0; attempt < 50; ++attempt) {
        const std::string value = command(replica_reader,
                                          {"GET", "replica-key"});
        if (value == "$10\r\nreplicated\r\n") {
            replicated = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(replicated, "replica receives primary SET");
    check(command(replica_reader, {"GET", "delete-key"}) == "$-1\r\n",
          "replica receives primary DEL");
    check(command(replica_reader, {"GET", "expiring-key"}) ==
              "$11\r\nshort-lived\r\n",
          "replica receives primary EXPIRE");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    check(command(replica_reader, {"GET", "expiring-key"}) == "$-1\r\n",
          "replicated expiration removes key");
    check(command(replica_reader, {"SET", "local", "write"}) ==
              "-READONLY replica accepts writes only from its primary\r\n",
          "replica rejects local writes");
    close(replica_reader);
    close(primary);
    close(replica_admin);
}

}  // namespace

int main() {
    const int primary_port = free_port();
    int replica_port = free_port();
    while (replica_port == primary_port) {
        replica_port = free_port();
    }
    check(primary_port > 0 && replica_port > 0, "allocate test ports");

    const auto root = std::filesystem::path("/tmp") /
        ("miniredis-network-" + std::to_string(getpid()));
    const auto primary_directory = root / "primary";
    const auto replica_directory = root / "replica";
    std::filesystem::create_directories(primary_directory);
    std::filesystem::create_directories(replica_directory);

    {
        ChildServer primary = start_server(primary_port, primary_directory);
        check(primary.pid > 0, "start primary server process");
        ChildServer replica = start_server(replica_port, replica_directory);
        check(replica.pid > 0, "start replica server process");
        test_tcp_commands(primary_port);
        test_pub_sub(primary_port);
        test_replication(primary_port, replica_port);
        test_persistence_error(primary_port, primary_directory);
    }

    std::filesystem::remove_all(root);
    std::cout << "Passed " << checks << " network checks" << std::endl;
}
