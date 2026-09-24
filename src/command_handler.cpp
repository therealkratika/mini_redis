#include "command_handler.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <utility>

namespace {

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    return value;
}

std::string wrong_argument_count(const std::string& command) {
    std::string lowercase = command;
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return "-ERR wrong number of arguments for '" + lowercase +
           "' command\r\n";
}

std::string bulk_string(const std::string& value) {
    return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string integer_reply(std::int64_t value) {
    return ":" + std::to_string(value) + "\r\n";
}

bool parse_integer(const std::string& text, std::int64_t& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return begin != end && result.ec == std::errc{} && result.ptr == end;
}

}  // namespace

CommandHandler::CommandHandler(KeyValueStore& storage,
                               AppendOnlyLog& persistence,
                               std::mutex& mutation_mutex, PubSub& pub_sub,
                               Replication& replication,
                               std::shared_ptr<ClientConnection> client)
    : storage(storage), persistence(persistence),
      mutation_mutex(mutation_mutex), pub_sub(pub_sub),
      replication(replication), client(std::move(client)) {}

std::string CommandHandler::handle(
    const std::vector<std::string>& arguments) {
    if (arguments.empty()) {
        return "-ERR empty command\r\n";
    }

    if (replication.is_replica_connection(client)) {
        return replication.apply_replicated(arguments)
            ? std::string{}
            : "-ERR invalid replicated command\r\n";
    }

    const std::string command = uppercase(arguments[0]);
    if (command == "REPLICA") {
        if (arguments.size() != 1) {
            return wrong_argument_count(arguments[0]);
        }
        return replication.register_replica(client)
            ? std::string{}
            : "-ERR replication handshake failed\r\n";
    }

    if (command == "REPLICAOF") {
        if (arguments.size() != 3) {
            return wrong_argument_count(arguments[0]);
        }
        std::int64_t port = 0;
        if (!parse_integer(arguments[2], port) || port < 1 || port > 65535) {
            return "-ERR invalid primary port\r\n";
        }
        std::string error;
        if (!replication.become_replica(arguments[1], arguments[2], error)) {
            return "-ERR " + error + "\r\n";
        }
        return "+OK\r\n";
    }

    if (command == "PING") {
        if (arguments.size() == 1) {
            return "+PONG\r\n";
        }
        if (arguments.size() == 2) {
            return bulk_string(arguments[1]);
        }
        return wrong_argument_count(arguments[0]);
    }

    if (command == "SUBSCRIBE") {
        if (arguments.size() < 2) {
            return wrong_argument_count(arguments[0]);
        }

        for (std::size_t i = 1; i < arguments.size(); ++i) {
            std::size_t count = 0;
            if (!pub_sub.subscribe(arguments[i], client, count)) {
                return "-ERR subscriber connection closed\r\n";
            }
        }
        return {};
    }

    if (command == "PUBLISH") {
        if (arguments.size() != 3) {
            return wrong_argument_count(arguments[0]);
        }
        const std::size_t receivers =
            pub_sub.publish(arguments[1], arguments[2]);
        return integer_reply(static_cast<std::int64_t>(receivers));
    }

    if (command == "SET") {
        if (arguments.size() != 3) {
            return wrong_argument_count(arguments[0]);
        }
        if (replication.is_replica()) {
            return "-READONLY replica accepts writes only from its primary\r\n";
        }
        std::lock_guard<std::mutex> lock(mutation_mutex);
        if (!persistence.append({"SET", arguments[1], arguments[2]})) {
            return "-ERR persistence failure\r\n";
        }
        storage.set(arguments[1], arguments[2]);
        replication.forward({"SET", arguments[1], arguments[2]});
        return "+OK\r\n";
    }

    if (command == "GET") {
        if (arguments.size() != 2) {
            return wrong_argument_count(arguments[0]);
        }
        const auto value = storage.get(arguments[1]);
        return value ? bulk_string(*value) : "$-1\r\n";
    }

    if (command == "DEL") {
        if (arguments.size() < 2) {
            return wrong_argument_count(arguments[0]);
        }
        if (replication.is_replica()) {
            return "-READONLY replica accepts writes only from its primary\r\n";
        }
        std::lock_guard<std::mutex> lock(mutation_mutex);
        std::vector<std::string> logged_arguments{"DEL"};
        logged_arguments.insert(logged_arguments.end(), arguments.begin() + 1,
                                arguments.end());
        if (!persistence.append(logged_arguments)) {
            return "-ERR persistence failure\r\n";
        }
        std::size_t deleted = 0;
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            deleted += storage.del(arguments[i]);
        }
        replication.forward(logged_arguments);
        return integer_reply(static_cast<std::int64_t>(deleted));
    }

    if (command == "EXPIRE") {
        if (arguments.size() != 3) {
            return wrong_argument_count(arguments[0]);
        }

        std::int64_t seconds = 0;
        if (!parse_integer(arguments[2], seconds)) {
            return "-ERR value is not an integer or out of range\r\n";
        }

        if (replication.is_replica()) {
            return "-READONLY replica accepts writes only from its primary\r\n";
        }
        std::lock_guard<std::mutex> lock(mutation_mutex);
        if (!storage.exists(arguments[1])) {
            return integer_reply(0);
        }

        const auto now = std::chrono::system_clock::now();
        const auto maximum_seconds =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::time_point::max() - now).count();
        if (seconds > maximum_seconds) {
            return "-ERR value is not an integer or out of range\r\n";
        }
        const auto expiry = seconds <= 0
            ? now
            : now + std::chrono::seconds(seconds);
        const auto expiry_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                expiry.time_since_epoch()).count();
        if (!persistence.append(
                {"EXPIREAT", arguments[1], std::to_string(expiry_ms)})) {
            return "-ERR persistence failure\r\n";
        }
        const std::vector<std::string> replication_command{
            "EXPIREAT", arguments[1], std::to_string(expiry_ms)};
        storage.expire_at(arguments[1], expiry);
        replication.forward(replication_command);
        return integer_reply(1);
    }

    return "-ERR unknown command\r\n";
}
