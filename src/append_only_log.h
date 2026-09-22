#ifndef APPEND_ONLY_LOG_H
#define APPEND_ONLY_LOG_H

#include <mutex>
#include <string>
#include <vector>

class KeyValueStore;

class AppendOnlyLog {
public:
    explicit AppendOnlyLog(std::string path = "appendonly.aof");

    bool append(const std::vector<std::string>& arguments);
    bool replay(KeyValueStore& storage);

private:
    std::string path;
    std::mutex mutex;
};

#endif
