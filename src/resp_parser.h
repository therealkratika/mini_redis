#ifndef RESP_PARSER_H
#define RESP_PARSER_H

#include <cstddef>
#include <string>
#include <vector>

enum class RespParseResult {
    Complete,
    Incomplete,
    Invalid
};

RespParseResult parse_resp_command(const std::string& input,
                                   std::vector<std::string>& arguments,
                                   std::size_t& consumed);

#endif
