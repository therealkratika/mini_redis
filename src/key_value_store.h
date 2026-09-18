#ifndef KEY_VALUE_STORE_H
#define KEY_VALUE_STORE_H

#include <optional>
#include <mutex>
#include <string>
#include <unordered_map>

class KeyValueStore {
public:
    void set(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key) const;
    bool del(const std::string& key);
    bool exists(const std::string& key) const;
    void clear();

private:
    std::unordered_map<std::string, std::string> values;
    mutable std::mutex mutex;
};

#endif
