#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

#include "key_value_store.h"

#include <string>
#include <vector>

class CommandHandler {
public:
    explicit CommandHandler(KeyValueStore& storage);
    std::string handle(const std::vector<std::string>& arguments);

private:
    KeyValueStore& storage;
};

#endif
