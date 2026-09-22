#include "append_only_log.h"

#include "key_value_store.h"
#include "resp_parser.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>
#include <utility>

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

bool parse_timestamp(const std::string& text, std::int64_t& timestamp) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, timestamp);
    return result.ec == std::errc{} && result.ptr == end && begin != end;
}

bool replay_command(const std::vector<std::string>& arguments,
                    KeyValueStore& storage) {
    if (arguments.empty()) {
        return false;
    }

    if (arguments[0] == "SET" && arguments.size() == 3) {
        storage.set(arguments[1], arguments[2]);
        return true;
    }

    if (arguments[0] == "DEL" && arguments.size() >= 2) {
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            storage.del(arguments[i]);
        }
        return true;
    }

    if (arguments[0] == "EXPIREAT" && arguments.size() == 3) {
        std::int64_t timestamp_ms = 0;
        if (!parse_timestamp(arguments[2], timestamp_ms)) {
            return false;
        }
        const auto timestamp =
            std::chrono::system_clock::time_point(
                std::chrono::milliseconds(timestamp_ms));
        storage.expire_at(arguments[1], timestamp);
        return true;
    }

    return false;
}

}  // namespace

AppendOnlyLog::AppendOnlyLog(std::string path) : path(std::move(path)) {}

bool AppendOnlyLog::append(const std::vector<std::string>& arguments) {
    std::lock_guard<std::mutex> lock(mutex);
    std::error_code error;
    const bool file_exists = std::filesystem::exists(path, error);
    if (error) {
        std::cerr << "Unable to inspect append-only log " << path
                  << ": " << error.message() << std::endl;
        return false;
    }
    const std::uintmax_t original_size =
        file_exists ? std::filesystem::file_size(path, error) : 0;
    if (error) {
        std::cerr << "Unable to inspect append-only log " << path
                  << ": " << error.message() << std::endl;
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        std::cerr << "Unable to open append-only log for writing: "
                  << path << std::endl;
        return false;
    }

    output << encode_command(arguments);
    output.flush();
    if (!output) {
        output.close();
        std::error_code rollback_error;
        std::filesystem::resize_file(path, original_size, rollback_error);
        std::cerr << "Failed to write append-only log: " << path << std::endl;
        if (rollback_error) {
            std::cerr << "Unable to roll back partial append: "
                      << rollback_error.message() << std::endl;
        }
        return false;
    }
    return true;
}

bool AppendOnlyLog::replay(KeyValueStore& storage) {
    std::lock_guard<std::mutex> lock(mutex);
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        std::cerr << "Unable to inspect append-only log " << path
                  << ": " << error.message() << std::endl;
        return false;
    }
    if (!exists) {
        return true;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Unable to open append-only log for reading: "
                  << path << std::endl;
        return false;
    }

    const std::string contents((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    if (input.bad()) {
        std::cerr << "Failed to read append-only log: " << path << std::endl;
        return false;
    }

    std::size_t position = 0;
    while (position < contents.size()) {
        std::vector<std::string> arguments;
        std::size_t consumed = 0;
        const RespParseResult result =
            parse_resp_command(contents, arguments, consumed, position);
        if (result != RespParseResult::Complete || consumed == 0) {
            std::cerr << "Invalid or incomplete command in append-only log: "
                      << path << std::endl;
            return false;
        }
        if (!replay_command(arguments, storage)) {
            std::cerr << "Unsupported command in append-only log: "
                      << path << std::endl;
            return false;
        }
        position += consumed;
    }
    return true;
}
