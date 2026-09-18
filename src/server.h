#ifndef SERVER_H
#define SERVER_H

#include <memory>

class KeyValueStore;

class Server {
public:
    Server(int port);
    bool start();

private:
    int port;
    std::shared_ptr<KeyValueStore> storage;
};

#endif