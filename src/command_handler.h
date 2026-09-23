#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

#include "append_only_log.h"
#include "client_connection.h"
#include "key_value_store.h"
#include "pub_sub.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

class CommandHandler {
public:
    CommandHandler(KeyValueStore& storage, AppendOnlyLog& persistence,
                   std::mutex& mutation_mutex, PubSub& pub_sub,
                   std::shared_ptr<ClientConnection> client);
    std::string handle(const std::vector<std::string>& arguments);

private:
    KeyValueStore& storage;
    AppendOnlyLog& persistence;
    std::mutex& mutation_mutex;
    PubSub& pub_sub;
    std::shared_ptr<ClientConnection> client;
};

#endif
