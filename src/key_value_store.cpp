#include "key_value_store.h"

void KeyValueStore::set(const std::string& key, const std::string& value) {
    values[key] = value;
}

std::optional<std::string> KeyValueStore::get(const std::string& key) const {
    const auto value = values.find(key);
    if (value == values.end()) {
        return std::nullopt;
    }
    return value->second;
}

bool KeyValueStore::del(const std::string& key) {
    return values.erase(key) != 0;
}

bool KeyValueStore::exists(const std::string& key) const {
    return values.find(key) != values.end();
}

void KeyValueStore::clear() {
    values.clear();
}
