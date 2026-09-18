#include "command_handler.h"

#include <algorithm>
#include <cctype>

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

}  // namespace

CommandHandler::CommandHandler(KeyValueStore& storage) : storage(storage) {}

std::string CommandHandler::handle(
    const std::vector<std::string>& arguments) {
    if (arguments.empty()) {
        return "-ERR empty command\r\n";
    }

    const std::string command = uppercase(arguments[0]);
    if (command == "PING") {
        if (arguments.size() == 1) {
            return "+PONG\r\n";
        }
        if (arguments.size() == 2) {
            return bulk_string(arguments[1]);
        }
        return wrong_argument_count(arguments[0]);
    }

    if (command == "SET") {
        if (arguments.size() != 3) {
            return wrong_argument_count(arguments[0]);
        }
        storage.set(arguments[1], arguments[2]);
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
        std::size_t deleted = 0;
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            deleted += storage.del(arguments[i]);
        }
        return ":" + std::to_string(deleted) + "\r\n";
    }

    return "-ERR unknown command\r\n";
}
