#ifndef SERVER_H
#define SERVER_H

class Server {
public:
    Server(int port);
    bool start();

private:
    int port;
};

#endif