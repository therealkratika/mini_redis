#include "key_value_store.h"

#include <mutex>

void KeyValueStore::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex);
    values[key] = value;
}

std::optional<std::string> KeyValueStore::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto value = values.find(key);
    if (value == values.end()) {
        return std::nullopt;
    }
    return value->second;
}

bool KeyValueStore::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex);
    return values.erase(key) != 0;
}

bool KeyValueStore::exists(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex);
    return values.find(key) != values.end();
}

void KeyValueStore::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    values.clear();
}
