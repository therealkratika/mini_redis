#include "key_value_store.h"

#include <utility>

void KeyValueStore::set(const std::string& key, const std::string& value,
                        std::optional<std::chrono::milliseconds> ttl) {
    std::lock_guard<std::mutex> lock(mutex);
    Entry entry{value, std::nullopt};
    if (ttl) {
        const auto now = Clock::now();
        entry.expires_at = *ttl <= std::chrono::milliseconds::zero()
                               ? now
                               : now + *ttl;
    }
    values[key] = std::move(entry);
}

std::optional<std::string> KeyValueStore::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto entry = values.find(key);
    if (entry == values.end() || expire_if_needed(entry)) {
        return std::nullopt;
    }
    return entry->second.value;
}

bool KeyValueStore::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto entry = values.find(key);
    if (entry == values.end() || expire_if_needed(entry)) {
        return false;
    }
    values.erase(entry);
    return true;
}

bool KeyValueStore::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto entry = values.find(key);
    return entry != values.end() && !expire_if_needed(entry);
}

bool KeyValueStore::expire_if_needed(
    std::unordered_map<std::string, Entry>::iterator entry) {
    if (!entry->second.expires_at ||
        Clock::now() < *entry->second.expires_at) {
        return false;
    }
    values.erase(entry);
    return true;
}

void KeyValueStore::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    values.clear();
}
