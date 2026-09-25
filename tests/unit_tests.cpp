#include "append_only_log.h"
#include "client_connection.h"
#include "command_handler.h"
#include "key_value_store.h"
#include "pub_sub.h"
#include "replication.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

std::string read_bytes(int fd, std::size_t count) {
    std::string result;
    while (result.size() < count) {
        char buffer[256];
        const ssize_t received =
            recv(fd, buffer, count - result.size(), 0);
        if (received <= 0) {
            return {};
        }
        result.append(buffer, static_cast<std::size_t>(received));
    }
    return result;
}

void test_storage() {
    using namespace std::chrono_literals;
    KeyValueStore storage;
    storage.set("key", "value");
    check(storage.exists("key"), "storage exists for stored key");
    check(storage.get("key") == "value", "storage returns stored value");
    check(storage.ttl_seconds("key") == -1, "persistent key has TTL -1");
    check(!storage.del("missing"), "deleting missing key returns false");
    check(storage.del("key"), "deleting stored key returns true");
    check(!storage.exists("key"), "deleted key is absent");

    storage.set("empty", "");
    check(storage.get("empty").has_value(), "empty value is distinguishable");
    storage.set("expires", "soon", 20ms);
    std::this_thread::sleep_for(35ms);
    check(!storage.get("expires"), "expired key is removed on access");
    check(storage.ttl_seconds("expires") == -2,
          "expired key has TTL -2");
    storage.set("clear-one", "1");
    storage.set("clear-two", "2");
    storage.clear();
    check(!storage.exists("clear-one") && !storage.exists("clear-two"),
          "clear removes all keys");
}

void test_commands_and_persistence() {
    const std::string path =
        "/tmp/miniredis-unit-aof-" + std::to_string(getpid());
    unlink(path.c_str());

    KeyValueStore storage;
    AppendOnlyLog log(path);
    auto shared_storage = std::make_shared<KeyValueStore>();
    auto shared_log = std::make_shared<AppendOnlyLog>(path);
    auto mutation_mutex = std::make_shared<std::mutex>();
    auto replication = std::make_shared<Replication>(
        shared_storage, shared_log, mutation_mutex);
    PubSub pub_sub;
    int sockets[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
          "create command test socket pair");
    auto client = std::make_shared<ClientConnection>(sockets[0]);
    CommandHandler handler(storage, log, *mutation_mutex, pub_sub,
                           *replication, client);

    check(handler.handle({"SET", "name", "Kratika"}) == "+OK\r\n",
          "SET returns Redis OK");
    check(handler.handle({"GET", "name"}) == "$7\r\nKratika\r\n",
          "GET returns RESP bulk string");
    check(handler.handle({"TTL", "name"}) == ":-1\r\n",
          "TTL returns -1 without expiry");
    check(handler.handle({"EXPIRE", "name", "30"}) == ":1\r\n",
          "EXPIRE succeeds for existing key");
    const std::string ttl_reply = handler.handle({"TTL", "name"});
    check(ttl_reply == ":30\r\n" || ttl_reply == ":29\r\n",
          "TTL reports remaining seconds");
    check(handler.handle({"DEL", "name"}) == ":1\r\n",
          "DEL returns number of keys removed");
    check(handler.handle({"GET", "name"}) == "$-1\r\n",
          "GET returns null bulk string after delete");

    check(log.append({"SET", "persist", "recovered"}),
          "append persistence SET record");
    check(log.append({"EXPIREAT", "persist", "4102444800000"}),
          "append persistence EXPIREAT record");
    check(log.append({"SET", "deleted", "no"}),
          "append persistence second SET record");
    check(log.append({"DEL", "deleted"}),
          "append persistence DEL record");

    KeyValueStore recovered;
    AppendOnlyLog recovery_log(path);
    check(recovery_log.replay(recovered), "replay append-only log");
    check(recovered.get("persist") == "recovered",
          "replay restores stored value");
    check(recovered.ttl_seconds("persist") > 0,
          "replay restores absolute expiration");
    check(!recovered.exists("deleted"), "replay applies deletion");

    client->close();
    close(sockets[1]);
    unlink(path.c_str());
}

void test_pub_sub() {
    int sockets[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
          "create Pub/Sub socket pair");
    auto client = std::make_shared<ClientConnection>(sockets[0]);
    PubSub pub_sub;
    std::size_t count = 0;
    check(pub_sub.subscribe("news", client, count),
          "subscribe sends acknowledgement");
    check(count == 1, "subscription count is one");
    const std::string expected_ack =
        "*3\r\n$9\r\nsubscribe\r\n$4\r\nnews\r\n:1\r\n";
    const std::string ack = read_bytes(sockets[1], expected_ack.size());
    check(ack == expected_ack,
          "subscription acknowledgement is RESP");
    check(pub_sub.publish("news", "hello") == 1,
          "publish returns subscriber count");
    const std::string expected_message =
        "*3\r\n$7\r\nmessage\r\n$4\r\nnews\r\n$5\r\nhello\r\n";
    check(read_bytes(sockets[1], expected_message.size()) == expected_message,
          "publish sends RESP message frame");
    pub_sub.remove_client(client);
    check(pub_sub.publish("news", "gone") == 0,
          "removed client is no longer subscribed");
    client->close();
    close(sockets[1]);
}

void test_replication_apply() {
    const std::string path =
        "/tmp/miniredis-repl-unit-aof-" + std::to_string(getpid());
    unlink(path.c_str());
    auto storage = std::make_shared<KeyValueStore>();
    auto log = std::make_shared<AppendOnlyLog>(path);
    auto mutex = std::make_shared<std::mutex>();
    Replication replication(storage, log, mutex);

    check(replication.apply_replicated({"SET", "replicated", "value"}),
          "replication applies SET");
    check(replication.apply_replicated({"EXPIREAT", "replicated",
                                        "4102444800000"}),
          "replication applies absolute expiry");
    check(storage->get("replicated") == "value",
          "replicated value is present");
    check(replication.apply_replicated({"DEL", "replicated"}),
          "replication applies DEL");
    check(!storage->exists("replicated"), "replicated deletion removes key");

    KeyValueStore recovered;
    AppendOnlyLog recovery_log(path);
    check(recovery_log.replay(recovered),
          "replication log replays successfully");
    check(!recovered.exists("replicated"),
          "replicated log preserves final deleted state");
    unlink(path.c_str());
}

}  // namespace

int main() {
    test_storage();
    test_commands_and_persistence();
    test_pub_sub();
    test_replication_apply();
    std::cout << "Passed " << checks << " unit checks" << std::endl;
}
