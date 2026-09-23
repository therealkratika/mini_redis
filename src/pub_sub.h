#ifndef PUB_SUB_H
#define PUB_SUB_H

#include "client_connection.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

class PubSub {
public:
    bool subscribe(const std::string& channel,
                   const std::shared_ptr<ClientConnection>& client,
                   std::size_t& subscription_count);
    std::size_t publish(const std::string& channel,
                        const std::string& message);
    void remove_client(const std::shared_ptr<ClientConnection>& client);

private:
    std::mutex mutex;
    std::unordered_map<
        std::string,
        std::unordered_map<ClientConnection*, std::shared_ptr<ClientConnection>>>
        subscribers;
    std::unordered_map<ClientConnection*, std::unordered_set<std::string>>
        channels_by_client;
};

#endif
