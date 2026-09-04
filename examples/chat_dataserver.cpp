#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

struct Message {
    std::string sender_id;
    std::string content;
    std::time_t timestamp = 0;
};

static std::string sanitize(std::string value) {
    for (char& character : value) {
        if (character == '\n' || character == '\r' || character == '|') {
            character = ' ';
        }
    }
    return value;
}

class SingleChannel {
public:
    void update_message(const std::string& sender_id,
                        const std::string& content) {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back({sanitize(sender_id), sanitize(content), std::time(nullptr)});
        if (messages_.size() > max_history_) {
            messages_.pop_front();
        }
    }

    std::string list_messages() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream output;
        for (const Message& message : messages_) {
            output << message.timestamp << ':' << message.sender_id << ':'
                   << message.content << '|';
        }
        return output.str();
    }

private:
    static constexpr std::size_t max_history_ = 100;
    mutable std::mutex mutex_;
    std::list<Message> messages_;
};

class ChannelManager {
public:
    std::shared_ptr<SingleChannel> get_channel(const std::string& channel_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& channel = channels_[channel_id];
        if (!channel) {
            channel = std::make_shared<SingleChannel>();
        }
        return channel;
    }

private:
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<SingleChannel>> channels_;
};

static bool send_all(int fd, const std::string& data) {
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        ssize_t sent = send(fd, cursor, remaining, MSG_NOSIGNAL);
        if (sent > 0) {
            cursor += sent;
            remaining -= static_cast<std::size_t>(sent);
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool receive_line(int fd, std::string& line) {
    line.clear();
    char character = 0;
    while (line.size() < 64 * 1024) {
        ssize_t received = recv(fd, &character, 1, 0);
        if (received == 1) {
            if (character == '\n') {
                return true;
            }
            if (character != '\r') {
                line.push_back(character);
            }
        } else if (received < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return false;
}

static void handle_client(int fd, ChannelManager& channels) {
    std::string line;
    while (receive_line(fd, line)) {
        if (line.rfind("get_channel:", 0) == 0) {
            std::string channel_id = line.substr(std::strlen("get_channel:"));
            std::string messages = channels.get_channel(channel_id)->list_messages();
            if (!send_all(fd, "history:" + messages + "\n")) {
                break;
            }
            continue;
        }

        if (line.rfind("send_message:", 0) == 0) {
            std::string payload = line.substr(std::strlen("send_message:"));
            std::size_t first = payload.find(':');
            std::size_t second = first == std::string::npos
                                     ? std::string::npos
                                     : payload.find(':', first + 1);
            if (first == std::string::npos || second == std::string::npos) {
                send_all(fd, "error:bad send_message request\n");
                continue;
            }

            std::string channel_id = payload.substr(0, first);
            std::string sender_id = payload.substr(first + 1, second - first - 1);
            std::string content = payload.substr(second + 1);
            channels.get_channel(channel_id)->update_message(sender_id, content);
            if (!send_all(fd, "ok\n")) {
                break;
            }
            continue;
        }

        if (line == "quit") {
            break;
        }
        if (!send_all(fd, "error:unknown command\n")) {
            break;
        }
    }
    close(fd);
}

int main(int argc, char** argv) {
    int port = argc > 1 ? std::atoi(argv[1]) : 9001;
    std::signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (server_fd < 0 ||
        bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(server_fd, SOMAXCONN) < 0) {
        std::perror("dataserver setup");
        return EXIT_FAILURE;
    }

    ChannelManager channels;
    std::cout << "DataServer listening on 0.0.0.0:" << port << std::endl;
    for (;;) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("accept");
            break;
        }
        std::thread(handle_client, client_fd, std::ref(channels)).detach();
    }

    close(server_fd);
    return EXIT_SUCCESS;
}
