#include "resp_parser.h"

#include <charconv>
#include <system_error>
#include <utility>

namespace {

RespParseResult parse_length(const std::string& input, std::size_t& position,
                             std::size_t& length) {
    const std::size_t line_end = input.find("\r\n", position);
    if (line_end == std::string::npos) {
        return RespParseResult::Incomplete;
    }

    const char* begin = input.data() + position;
    const char* end = input.data() + line_end;
    if (begin == end) {
        return RespParseResult::Invalid;
    }
    const auto result = std::from_chars(begin, end, length);
    if (result.ec != std::errc{} || result.ptr != end) {
        return RespParseResult::Invalid;
    }

    position = line_end + 2;
    return RespParseResult::Complete;
}

}  // namespace

RespParseResult parse_resp_command(const std::string& input,
                                   std::vector<std::string>& arguments,
                                   std::size_t& consumed) {
    consumed = 0;
    if (input.empty()) {
        return RespParseResult::Incomplete;
    }
    if (input[0] != '*') {
        return RespParseResult::Invalid;
    }

    std::size_t position = 1;
    std::size_t argument_count = 0;
    RespParseResult result = parse_length(input, position, argument_count);
    if (result != RespParseResult::Complete) {
        return result;
    }

    std::vector<std::string> parsed_arguments;
    for (std::size_t i = 0; i < argument_count; ++i) {
        if (position == input.size()) {
            return RespParseResult::Incomplete;
        }
        if (input[position] != '$') {
            return RespParseResult::Invalid;
        }
        ++position;

        std::size_t bulk_length = 0;
        result = parse_length(input, position, bulk_length);
        if (result != RespParseResult::Complete) {
            return result;
        }

        if (bulk_length > input.size() - position ||
            input.size() - position - bulk_length < 2) {
            return RespParseResult::Incomplete;
        }
        if (input[position + bulk_length] != '\r' ||
            input[position + bulk_length + 1] != '\n') {
            return RespParseResult::Invalid;
        }

        parsed_arguments.emplace_back(input.data() + position, bulk_length);
        position += bulk_length + 2;
    }

    arguments = std::move(parsed_arguments);
    consumed = position;
    return RespParseResult::Complete;
}
