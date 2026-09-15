#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

#include <string>
#include <unordered_map>
#include <vector>

class CommandHandler {
public:
    std::string handle(const std::vector<std::string>& arguments);

private:
    std::unordered_map<std::string, std::string> values;
};

#endif
