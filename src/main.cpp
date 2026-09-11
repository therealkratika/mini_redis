#include "server.h"

int main() {
    Server server(6380);
    return server.start() ? 0 : 1;
}