#include <arpa/inet.h>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

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

class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count) {
        for (std::size_t index = 0; index < thread_count; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        for (std::thread& worker : workers_) {
            worker.join();
        }
    }

    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push(std::move(task));
        }
        changed_.notify_one();
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

struct ClientInfo {
    int fd = -1;
    std::string client_id;
    std::string current_channel;
};

class ClientManager {
public:
    void add_connection(int fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        clients_[fd].fd = fd;
    }

    bool login(int fd, const std::string& client_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [other_fd, client] : clients_) {
            if (other_fd != fd && client.client_id == client_id) {
                return false;
            }
        }
        clients_[fd].client_id = client_id;
        return true;
    }

    bool set_channel(int fd, const std::string& channel_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = clients_.find(fd);
        if (found == clients_.end() || found->second.client_id.empty()) {
            return false;
        }
        found->second.current_channel = channel_id;
        return true;
    }

    std::optional<ClientInfo> get(int fd) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = clients_.find(fd);
        return found == clients_.end()
                   ? std::nullopt
                   : std::optional<ClientInfo>(found->second);
    }

    std::vector<int> clients_in_channel(const std::string& channel_id) const {
        std::vector<int> result;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [fd, client] : clients_) {
            if (client.current_channel == channel_id) {
                result.push_back(fd);
            }
        }
        return result;
    }

    std::optional<ClientInfo> remove(int fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = clients_.find(fd);
        if (found == clients_.end()) {
            return std::nullopt;
        }
        ClientInfo removed = found->second;
        clients_.erase(found);
        return removed;
    }

private:
    mutable std::mutex mutex_;
    std::map<int, ClientInfo> clients_;
};

class DataServerClient {
public:
    DataServerClient(std::string host, int port)
        : host_(std::move(host)), port_(port) {}

    ~DataServerClient() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    std::optional<std::string> request(const std::string& request_line) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (fd_ < 0 && !connect_locked()) {
                continue;
            }
            if (send_all(fd_, request_line + "\n")) {
                std::string response;
                if (receive_line(fd_, response)) {
                    return response;
                }
            }
            close(fd_);
            fd_ = -1;
        }
        return std::nullopt;
    }

private:
    bool connect_locked() {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return false;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port_));
        if (inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1 ||
            connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
    }

    std::string host_;
    int port_;
    int fd_ = -1;
    std::mutex mutex_;
};

static void broadcast(ClientManager& clients,
                      const std::string& channel_id,
                      const std::string& message,
                      int excluded_fd = -1) {
    for (int fd : clients.clients_in_channel(channel_id)) {
        if (fd != excluded_fd) {
            send_all(fd, message + "\n");
        }
    }
}

static void handle_client(int fd,
                          ClientManager& clients,
                          DataServerClient& dataserver) {
    clients.add_connection(fd);
    send_all(fd,
             "commands: login:<id>, join_channel:<channel>, "
             "send_message:<text>, quit\n");

    std::string command;
    while (receive_line(fd, command)) {
        if (command.rfind("login:", 0) == 0) {
            std::string client_id = command.substr(std::strlen("login:"));
            if (client_id.empty()) {
                send_all(fd, "error:empty client id\n");
            } else if (!clients.login(fd, client_id)) {
                send_all(fd, "error:nickname already used\n");
            } else {
                send_all(fd, "ok:login\n");
            }
            continue;
        }

        if (command.rfind("join_channel:", 0) == 0) {
            std::string channel_id = command.substr(std::strlen("join_channel:"));
            auto client = clients.get(fd);
            if (!client || client->client_id.empty() || channel_id.empty()) {
                send_all(fd, "error:login first or invalid channel\n");
                continue;
            }
            if (!clients.set_channel(fd, channel_id)) {
                send_all(fd, "error:cannot join channel\n");
                continue;
            }

            auto history = dataserver.request("get_channel:" + channel_id);
            if (!history) {
                send_all(fd, "error:dataserver unavailable\n");
                continue;
            }
            send_all(fd, *history + "\n");
            broadcast(clients, channel_id,
                      "system:" + client->client_id + " joined " + channel_id,
                      fd);
            continue;
        }

        if (command.rfind("send_message:", 0) == 0) {
            auto client = clients.get(fd);
            std::string content = command.substr(std::strlen("send_message:"));
            if (!client || client->client_id.empty() ||
                client->current_channel.empty() || content.empty()) {
                send_all(fd, "error:join a channel before sending\n");
                continue;
            }

            auto result = dataserver.request(
                "send_message:" + client->current_channel + ':' +
                client->client_id + ':' + content);
            if (!result || *result != "ok") {
                send_all(fd, "error:dataserver rejected message\n");
                continue;
            }

            const std::string delivered =
                "message:" + std::to_string(std::time(nullptr)) + ':' +
                client->client_id + ':' + content;
            broadcast(clients, client->current_channel, delivered);
            continue;
        }

        if (command == "quit") {
            break;
        }
        send_all(fd, "error:unknown command\n");
    }

    auto removed = clients.remove(fd);
    if (removed && !removed->current_channel.empty()) {
        broadcast(clients, removed->current_channel,
                  "system:" + removed->client_id + " left",
                  fd);
    }
    close(fd);
}

int main(int argc, char** argv) {
    int listen_port = argc > 1 ? std::atoi(argv[1]) : 9000;
    std::string data_host = argc > 2 ? argv[2] : "127.0.0.1";
    int data_port = argc > 3 ? std::atoi(argv[3]) : 9001;
    std::signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(listen_port));
    if (server_fd < 0 ||
        bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(server_fd, SOMAXCONN) < 0) {
        std::perror("logicserver setup");
        return EXIT_FAILURE;
    }

    ClientManager clients;
    DataServerClient dataserver(data_host, data_port);
    ThreadPool workers(8);
    std::cout << "LogicServer listening on 0.0.0.0:" << listen_port
              << ", DataServer=" << data_host << ':' << data_port << std::endl;

    for (;;) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("accept");
            break;
        }
        workers.submit([client_fd, &clients, &dataserver] {
            handle_client(client_fd, clients, dataserver);
        });
    }

    close(server_fd);
    return EXIT_SUCCESS;
}
