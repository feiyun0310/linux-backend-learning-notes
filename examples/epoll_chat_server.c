#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    DEFAULT_PORT = 8888,
    MAX_EVENTS = 128,
    MAX_CLIENTS = 100,
    MAX_NAME = 32,
    INPUT_CAPACITY = 4096,
    MESSAGE_CAPACITY = 2048,
    HISTORY_CAPACITY = 10,
};

typedef struct {
    int fd;
    int active;
    int nickname_ready;
    char nickname[MAX_NAME];
    char input[INPUT_CAPACITY];
    size_t input_length;
} Client;

static Client clients[MAX_CLIENTS];
static char history[HISTORY_CAPACITY][MESSAGE_CAPACITY];
static size_t history_size = 0;
static size_t history_next = 0;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int send_all(int fd, const char* data, size_t length) {
    while (length > 0) {
        ssize_t sent = send(fd, data, length, MSG_NOSIGNAL);
        if (sent > 0) {
            data += sent;
            length -= (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int send_text(int fd, const char* text) {
    return send_all(fd, text, strlen(text));
}

static void current_time(char* buffer, size_t capacity) {
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(buffer, capacity, "%H:%M:%S", &local);
}

static Client* find_client(int fd) {
    for (size_t index = 0; index < MAX_CLIENTS; ++index) {
        if (clients[index].active && clients[index].fd == fd) {
            return &clients[index];
        }
    }
    return NULL;
}

static Client* add_client(int fd) {
    for (size_t index = 0; index < MAX_CLIENTS; ++index) {
        if (!clients[index].active) {
            clients[index] = (Client){.fd = fd, .active = 1};
            return &clients[index];
        }
    }
    return NULL;
}

static int nickname_exists(const char* nickname) {
    for (size_t index = 0; index < MAX_CLIENTS; ++index) {
        if (clients[index].active && clients[index].nickname_ready &&
            strcmp(clients[index].nickname, nickname) == 0) {
            return 1;
        }
    }
    return 0;
}

static void add_history(const char* message) {
    snprintf(history[history_next], MESSAGE_CAPACITY, "%s", message);
    history_next = (history_next + 1) % HISTORY_CAPACITY;
    if (history_size < HISTORY_CAPACITY) {
        ++history_size;
    }
}

static void broadcast(const char* message, int excluded_fd) {
    for (size_t index = 0; index < MAX_CLIENTS; ++index) {
        if (clients[index].active && clients[index].nickname_ready &&
            clients[index].fd != excluded_fd) {
            send_all(clients[index].fd, message, strlen(message));
        }
    }
}

static void send_history(int fd) {
    send_text(fd, "=== recent messages ===\n");
    if (history_size == 0) {
        send_text(fd, "(no messages)\n");
    } else {
        size_t start =
            (history_next + HISTORY_CAPACITY - history_size) % HISTORY_CAPACITY;
        for (size_t offset = 0; offset < history_size; ++offset) {
            const char* message = history[(start + offset) % HISTORY_CAPACITY];
            send_all(fd, message, strlen(message));
        }
    }
    send_text(fd, "=== end history ===\nNICK> ");
}

static void disconnect_client(int epoll_fd, Client* client, int announce) {
    char nickname[MAX_NAME] = "";
    if (client->nickname_ready) {
        snprintf(nickname, sizeof(nickname), "%s", client->nickname);
    }

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
    close(client->fd);
    memset(client, 0, sizeof(*client));

    if (announce && nickname[0] != '\0') {
        char timestamp[16];
        char message[MESSAGE_CAPACITY];
        current_time(timestamp, sizeof(timestamp));
        snprintf(message, sizeof(message), "[%s] *** %s left ***\n",
                 timestamp, nickname);
        add_history(message);
        broadcast(message, -1);
        fputs(message, stdout);
    }
}

static int process_line(int epoll_fd, Client* client, char* line) {
    size_t length = strlen(line);
    if (length > 0 && line[length - 1] == '\r') {
        line[length - 1] = '\0';
    }

    if (!client->nickname_ready) {
        if (line[0] == '\0' || strlen(line) >= MAX_NAME) {
            send_text(client->fd, "ERR invalid nickname\nNICK> ");
            return 1;
        }
        if (nickname_exists(line)) {
            send_text(client->fd, "ERR nickname already used\nNICK> ");
            return 1;
        }

        snprintf(client->nickname, sizeof(client->nickname), "%s", line);
        client->nickname_ready = 1;
        send_text(client->fd, "OK nickname accepted\n");

        char timestamp[16];
        char message[MESSAGE_CAPACITY];
        current_time(timestamp, sizeof(timestamp));
        snprintf(message, sizeof(message), "[%s] *** %s joined ***\n",
                 timestamp, client->nickname);
        add_history(message);
        broadcast(message, -1);
        fputs(message, stdout);
        return 1;
    }

    if (strcmp(line, "/quit") == 0) {
        disconnect_client(epoll_fd, client, 1);
        return 0;
    }
    if (line[0] == '\0') {
        return 1;
    }

    char timestamp[16];
    char message[MESSAGE_CAPACITY];
    current_time(timestamp, sizeof(timestamp));
    snprintf(message, sizeof(message), "[%s] %s: %s\n",
             timestamp, client->nickname, line);
    add_history(message);
    broadcast(message, client->fd);
    fputs(message, stdout);
    return 1;
}

static int read_client(int epoll_fd, Client* client) {
    char incoming[1024];
    for (;;) {
        ssize_t received = recv(client->fd, incoming, sizeof(incoming), 0);
        if (received == 0) {
            disconnect_client(epoll_fd, client, 1);
            return 0;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 1;
            }
            disconnect_client(epoll_fd, client, 1);
            return 0;
        }

        for (ssize_t index = 0; index < received; ++index) {
            char value = incoming[index];
            if (value == '\n') {
                client->input[client->input_length] = '\0';
                if (!process_line(epoll_fd, client, client->input)) {
                    return 0;
                }
                client->input_length = 0;
            } else if (client->input_length + 1 < INPUT_CAPACITY) {
                client->input[client->input_length++] = value;
            } else {
                send_text(client->fd, "ERR line too long\n");
                disconnect_client(epoll_fd, client, 1);
                return 0;
            }
        }
    }
}

int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;
    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (set_nonblocking(server_fd) < 0) {
        perror("fcntl");
        return EXIT_FAILURE;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0 ||
        listen(server_fd, SOMAXCONN) < 0) {
        perror("bind/listen");
        return EXIT_FAILURE;
    }

    int epoll_fd = epoll_create1(0);
    struct epoll_event event = {.events = EPOLLIN, .data.fd = server_fd};
    if (epoll_fd < 0 ||
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) < 0) {
        perror("epoll setup");
        return EXIT_FAILURE;
    }

    printf("epoll chat server listening on 0.0.0.0:%d\n", port);
    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int ready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int index = 0; index < ready; ++index) {
            int fd = events[index].data.fd;
            if (fd == server_fd) {
                for (;;) {
                    int client_fd = accept(server_fd, NULL, NULL);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        perror("accept");
                        break;
                    }

                    Client* client = add_client(client_fd);
                    if (!client || set_nonblocking(client_fd) < 0) {
                        send_text(client_fd, "ERR server full\n");
                        close(client_fd);
                        continue;
                    }

                    struct epoll_event client_event = {
                        .events = EPOLLIN | EPOLLRDHUP,
                        .data.fd = client_fd,
                    };
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD,
                                  client_fd, &client_event) < 0) {
                        perror("epoll_ctl client");
                        disconnect_client(epoll_fd, client, 0);
                        continue;
                    }
                    send_text(client_fd, "=== welcome ===\n");
                    send_history(client_fd);
                }
            } else {
                Client* client = find_client(fd);
                if (!client) {
                    continue;
                }
                if (events[index].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    disconnect_client(epoll_fd, client, 1);
                    continue;
                }
                read_client(epoll_fd, client);
            }
        }
    }

    close(epoll_fd);
    close(server_fd);
    return EXIT_SUCCESS;
}
