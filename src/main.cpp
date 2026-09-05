#include "server.h"

int main() {
    Server server(6380);
    server.start();

    return 0;
}