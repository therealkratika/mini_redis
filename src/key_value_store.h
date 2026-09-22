#ifndef KEY_VALUE_STORE_H
#define KEY_VALUE_STORE_H

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

class KeyValueStore {
public:
    void set(const std::string& key, const std::string& value,
             std::optional<std::chrono::milliseconds> ttl = std::nullopt);
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool exists(const std::string& key);
    bool expire_at(const std::string& key,
                   std::chrono::system_clock::time_point expires_at);
    void clear();

private:
    using Clock = std::chrono::steady_clock;

    struct Entry {
        std::string value;
        std::optional<Clock::time_point> expires_at;
    };

    bool expire_if_needed(
        std::unordered_map<std::string, Entry>::iterator entry);

    std::unordered_map<std::string, Entry> values;
    std::mutex mutex;
};

#endif
