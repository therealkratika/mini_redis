#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr char key_prefix[] = "miniredis-bench:";
const std::string benchmark_value = "benchmark-value";

struct Options {
    std::string host = "localhost";
    std::string port = "6380";
    std::size_t requests = 10000;
    std::size_t clients = 1;
};

struct Result {
    double requests_per_second = 0;
    double average_latency_us = 0;
    std::size_t completed = 0;
};

bool parse_size(const char* text, std::size_t& value) {
    const std::string input(text);
    const char* begin = input.data();
    const char* end = begin + input.size();
    const auto parsed = std::from_chars(begin, end, value);
    return begin != end && parsed.ec == std::errc{} && parsed.ptr == end &&
           value > 0;
}

bool parse_options(int argc, char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string option(argv[i]);
        if (option == "--help") {
            std::cout << "Usage: miniredis-benchmark [--host HOST] "
                         "[--port PORT] [--requests COUNT] [--clients COUNT]\n";
            return false;
        }
        if (i + 1 == argc) {
            std::cerr << "Missing value for " << option << '\n';
            return false;
        }

        const char* value = argv[++i];
        if (option == "--host") {
            options.host = value;
        } else if (option == "--port") {
            std::size_t port_number = 0;
            if (!parse_size(value, port_number) || port_number > 65535) {
                std::cerr << "Invalid port: " << value << '\n';
                return false;
            }
            options.port = value;
        } else if (option == "--requests") {
            if (!parse_size(value, options.requests)) {
                std::cerr << "Invalid request count: " << value << '\n';
                return false;
            }
        } else if (option == "--clients") {
            if (!parse_size(value, options.clients)) {
                std::cerr << "Invalid client count: " << value << '\n';
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << option << '\n';
            return false;
        }
    }

    if (options.clients > options.requests) {
        options.clients = options.requests;
    }
    return true;
}

int connect_to_server(const Options& options) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int lookup = getaddrinfo(options.host.c_str(), options.port.c_str(),
                                   &hints, &addresses);
    if (lookup != 0) {
        std::cerr << "Unable to resolve server: " << gai_strerror(lookup)
                  << '\n';
        return -1;
    }

    int fd = -1;
    for (addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
        if (fd == -1) {
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd == -1) {
        perror("connect");
    }
    return fd;
}

bool read_line(int fd, std::string& line) {
    line.clear();
    char character = 0;
    while (line.size() <= 1024 * 1024) {
        const ssize_t received = recv(fd, &character, 1, 0);
        if (received == -1 && errno == EINTR) {
            continue;
        }
        if (received != 1) {
            return false;
        }
        line += character;
        if (line.size() >= 2 &&
            line.compare(line.size() - 2, 2, "\r\n") == 0) {
            line.resize(line.size() - 2);
            return true;
        }
    }
    return false;
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
        constexpr int send_flags = MSG_NOSIGNAL;
#else
        constexpr int send_flags = 0;
#endif
        const ssize_t result = send(fd, data.data() + sent,
                                    data.size() - sent, send_flags);
        if (result == -1 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool read_response(int fd, bool expect_bulk) {
    std::string line;
    if (!read_line(fd, line) || line.empty()) {
        return false;
    }
    if (!expect_bulk) {
        return line == "+OK";
    }

    if (line[0] != '$') {
        return false;
    }
    std::size_t length = 0;
    const char* begin = line.data() + 1;
    const char* end = line.data() + line.size();
    const auto parsed = std::from_chars(begin, end, length);
    if (begin == end || parsed.ec != std::errc{} || parsed.ptr != end) {
        return false;
    }

    std::string body(length + 2, '\0');
    std::size_t received_total = 0;
    while (received_total < body.size()) {
        const ssize_t received =
            recv(fd, body.data() + received_total,
                 body.size() - received_total, 0);
        if (received == -1 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return false;
        }
        received_total += static_cast<std::size_t>(received);
    }
    return body.compare(length, 2, "\r\n") == 0 &&
           length == benchmark_value.size() &&
           body.compare(0, length, benchmark_value) == 0;
}

std::string encode_command(const std::string& command,
                           const std::string& key,
                           const std::string* command_value = nullptr) {
    const std::size_t count = command_value == nullptr ? 2 : 3;
    std::string request = "*" + std::to_string(count) + "\r\n";
    request += "$" + std::to_string(command.size()) + "\r\n" + command +
               "\r\n";
    request += "$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
    if (command_value != nullptr) {
        request += "$" + std::to_string(command_value->size()) + "\r\n" +
                   *command_value + "\r\n";
    }
    return request;
}

Result run_phase(const Options& options, bool is_set) {
    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<std::uint64_t> latency_ns{0};
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::mutex error_mutex;
    std::string error;
    std::vector<std::thread> workers;
    workers.reserve(options.clients);

    for (std::size_t client = 0; client < options.clients; ++client) {
        workers.emplace_back([&, client] {
            const int fd = connect_to_server(options);
            if (fd == -1) {
                failed.store(true);
                std::lock_guard<std::mutex> lock(error_mutex);
                error = "Unable to connect benchmark client to server";
                ++ready;
                return;
            }

            std::string greeting;
            if (!read_line(fd, greeting) ||
                greeting != "Hello from MiniRedis!") {
                failed.store(true);
                std::lock_guard<std::mutex> lock(error_mutex);
                error = "Invalid greeting from server";
                close(fd);
                ++ready;
                return;
            }
            ++ready;
            while (!start.load()) {
                std::this_thread::yield();
            }

            for (std::size_t index = client; index < options.requests;
                 index += options.clients) {
                const std::string key =
                    std::string(key_prefix) + std::to_string(index);
                const std::string request =
                    is_set ? encode_command("SET", key, &benchmark_value)
                           : encode_command("GET", key);
                const auto request_start = std::chrono::steady_clock::now();
                if (!send_all(fd, request) || !read_response(fd, !is_set)) {
                    failed.store(true);
                    std::lock_guard<std::mutex> lock(error_mutex);
                    error = "Server returned an invalid response during " +
                            std::string(is_set ? "SET" : "GET");
                    break;
                }
                const auto request_end = std::chrono::steady_clock::now();
                latency_ns += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        request_end - request_start).count());
                ++completed;
            }
            shutdown(fd, SHUT_RDWR);
            close(fd);
        });
    }

    while (ready.load() < options.clients) {
        std::this_thread::yield();
    }
    if (failed.load()) {
        start.store(true);
        for (auto& worker : workers) {
            worker.join();
        }
        std::cerr << error << '\n';
        return {};
    }

    const auto measured_start = std::chrono::steady_clock::now();
    start.store(true);
    for (auto& worker : workers) {
        worker.join();
    }
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      measured_start).count();

    if (failed.load()) {
        std::cerr << error << '\n';
        return {};
    }

    Result result;
    result.completed = completed.load();
    result.requests_per_second =
        static_cast<double>(result.completed) / elapsed;
    result.average_latency_us =
        static_cast<double>(latency_ns.load()) /
        static_cast<double>(result.completed) / 1000.0;
    return result;
}

void print_result(const char* name, const Result& result) {
    std::cout << name << ": " << result.requests_per_second
              << " requests/sec, average latency "
              << result.average_latency_us << " us ("
              << result.completed << " requests)\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return argc > 1 && std::string(argv[1]) == "--help" ? 0 : 2;
    }

    std::cout << "Benchmarking " << options.host << ':' << options.port
              << " with " << options.requests << " requests per phase and "
              << options.clients << " concurrent clients\n";

    const Result set_result = run_phase(options, true);
    if (set_result.completed != options.requests) {
        return 1;
    }
    const Result get_result = run_phase(options, false);
    if (get_result.completed != options.requests) {
        return 1;
    }

    print_result("SET", set_result);
    print_result("GET", get_result);
    return 0;
}
