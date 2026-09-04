#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { BUFFER_SIZE = 1024 };

static int server_fd = -1;
static atomic_int running = 1;

static int send_all(int fd, const char* data, size_t length) {
    while (length > 0) {
        ssize_t sent = send(fd, data, length, MSG_NOSIGNAL);
        if (sent > 0) {
            data += sent;
            length -= (size_t)sent;
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static void* receive_loop(void* unused) {
    (void)unused;
    char buffer[BUFFER_SIZE];
    while (atomic_load(&running)) {
        ssize_t received = recv(server_fd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            fwrite(buffer, 1, (size_t)received, stdout);
            fflush(stdout);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    atomic_store(&running, 0);
    fputs("\n[system] disconnected from server\n", stdout);
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <server-ip> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1 ||
        connect(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("connect");
        close(server_fd);
        return EXIT_FAILURE;
    }

    pthread_t receiver;
    if (pthread_create(&receiver, NULL, receive_loop, NULL) != 0) {
        perror("pthread_create");
        close(server_fd);
        return EXIT_FAILURE;
    }

    char line[BUFFER_SIZE];
    while (atomic_load(&running) && fgets(line, sizeof(line), stdin)) {
        if (send_all(server_fd, line, strlen(line)) < 0) {
            break;
        }
        if (strcmp(line, "/quit\n") == 0) {
            break;
        }
    }

    atomic_store(&running, 0);
    shutdown(server_fd, SHUT_RDWR);
    pthread_join(receiver, NULL);
    close(server_fd);
    return EXIT_SUCCESS;
}
