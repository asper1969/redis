#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

static void die(const char* msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void msg(const char* msg) {
    fprintf(stderr, "%s\n", msg);
}

static void do_something(int confd) {
    // Placeholder for future functionality
    char rbuf[64] = {};
    ssize_t n = read(confd, rbuf, sizeof(rbuf) - 1);
    
    if (n < 0) {
        msg("read() error");
        return;
    }

    printf("client says: %s\n", rbuf);

    char wbuf[] = "hello from server\n";
    ssize_t w = write(confd, wbuf, strlen(wbuf));

    if (w < 0) {
        msg("write() error");
        return;
    }
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234); // Use htons to convert port number to network byte order
    addr.sin_addr.s_addr = htonl(0); // wildcard IP 0.0.0.0
    int rv = bind(fd, (const struct sockaddr*)&addr, sizeof(addr));

    if (rv) {
        die("bind failed");
    }

    // listen
    rv = listen(fd, SOMAXCONN);

    if (rv) {
        die("listen failed");
    }

    while (true) {
        // accept
        struct sockaddr_in client_addr = {};
        socklen_t addrlen = sizeof(client_addr);
        int connfd = accept(fd, (struct sockaddr*)&client_addr, &addrlen);

        if (connfd < 0) {
            continue;
        }

        do_something(connfd);
        close(connfd);
    }

    return 0;
}