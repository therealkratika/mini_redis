#ifndef CLIENT_CONNECTION_H
#define CLIENT_CONNECTION_H

#include <mutex>
#include <string>

class ClientConnection {
public:
    explicit ClientConnection(int socket_fd);
    ~ClientConnection();

    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    int socket_fd() const;
    bool send(const std::string& data);
    void close();

private:
    int fd;
    bool closed = false;
    std::mutex send_mutex;
};

#endif
