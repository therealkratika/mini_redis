#ifndef REPLICATION_H
#define REPLICATION_H

#include "client_connection.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

class AppendOnlyLog;
class KeyValueStore;

class Replication {
public:
    Replication(std::shared_ptr<KeyValueStore> storage,
                std::shared_ptr<AppendOnlyLog> persistence,
                std::shared_ptr<std::mutex> mutation_mutex);

    bool register_replica(const std::shared_ptr<ClientConnection>& client);
    void remove_replica(const std::shared_ptr<ClientConnection>& client);
    bool is_replica_connection(
        const std::shared_ptr<ClientConnection>& client);
    void forward(const std::vector<std::string>& arguments);
    bool become_replica(const std::string& host, const std::string& port,
                        std::string& error);
    bool is_replica() const;
    bool apply_replicated(const std::vector<std::string>& arguments);

private:
    void receive_replication(
        const std::shared_ptr<ClientConnection>& upstream);

    std::shared_ptr<KeyValueStore> storage;
    std::shared_ptr<AppendOnlyLog> persistence;
    std::shared_ptr<std::mutex> mutation_mutex;
    mutable std::mutex mutex;
    std::mutex replica_setup_mutex;
    std::unordered_set<ClientConnection*> replica_clients;
    std::vector<std::shared_ptr<ClientConnection>> replicas;
    std::atomic<bool> replica_mode{false};
};

#endif
