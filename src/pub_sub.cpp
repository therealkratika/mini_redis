#include "pub_sub.h"

#include <vector>

namespace {

std::string bulk_string(const std::string& value) {
    return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string message_frame(const std::string& channel,
                          const std::string& message) {
    return "*3\r\n" + bulk_string("message") + bulk_string(channel) +
           bulk_string(message);
}

}  // namespace

bool PubSub::subscribe(
    const std::string& channel,
    const std::shared_ptr<ClientConnection>& client,
    std::size_t& subscription_count) {
    std::lock_guard<std::mutex> lock(mutex);
    auto& channels = channels_by_client[client.get()];
    if (channels.insert(channel).second) {
        subscribers[channel][client.get()] = client;
    }

    subscription_count = channels.size();
    const std::string acknowledgement =
        "*3\r\n" + bulk_string("subscribe") + bulk_string(channel) + ":" +
        std::to_string(subscription_count) + "\r\n";
    return client->send(acknowledgement);
}

std::size_t PubSub::publish(const std::string& channel,
                            const std::string& message) {
    std::vector<std::shared_ptr<ClientConnection>> clients;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto channel_subscribers = subscribers.find(channel);
        if (channel_subscribers == subscribers.end()) {
            return 0;
        }
        clients.reserve(channel_subscribers->second.size());
        for (const auto& subscriber : channel_subscribers->second) {
            clients.push_back(subscriber.second);
        }
    }

    const std::string frame = message_frame(channel, message);
    for (const auto& client : clients) {
        client->send(frame);
    }
    return clients.size();
}

void PubSub::remove_client(
    const std::shared_ptr<ClientConnection>& client) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto channels = channels_by_client.find(client.get());
    if (channels == channels_by_client.end()) {
        return;
    }

    for (const std::string& channel : channels->second) {
        const auto channel_subscribers = subscribers.find(channel);
        if (channel_subscribers == subscribers.end()) {
            continue;
        }
        channel_subscribers->second.erase(client.get());
        if (channel_subscribers->second.empty()) {
            subscribers.erase(channel_subscribers);
        }
    }
    channels_by_client.erase(channels);
}
