#ifndef SERVER_H
#define SERVER_H

#include <memory>
#include <mutex>

class AppendOnlyLog;
class KeyValueStore;
class PubSub;

class Server {
public:
    Server(int port);
    bool start();

private:
    int port;
    std::shared_ptr<KeyValueStore> storage;
    std::shared_ptr<AppendOnlyLog> persistence;
    std::shared_ptr<std::mutex> mutation_mutex;
    std::shared_ptr<PubSub> pub_sub;
};

#endif