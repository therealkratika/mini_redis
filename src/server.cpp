#include "server.h"

#include <iostream>

Server::Server(int port) : port(port) {}

void Server::start() {
    std::cout << "MiniRedis starting on port "
              << port << std::endl;
}