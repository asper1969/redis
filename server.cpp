#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <poll.h>
#include <assert.h>
#include <vector>
#include <fcntl.h>

static void msg(const char* msg) {
    fprintf(stderr, "%s\n", msg);
}

static void msg_errno(const char* msg) {
    fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

static void die(const char* msg) {
    fprintf(stderr, "[%d] %s\n", errno, msg);
    abort();
}

// make the listening socket non-blocking
static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);

    if (errno) {
        die("fcntl F_GETFL failed");
        return;
    }

    flags |= O_NONBLOCK;
    errno = 0;
    (void) fcntl(fd, F_SETFL, flags);

    if (errno) {
        die("fcntl F_SETFL failed");
        return;
    }
}

const size_t k_max_msg = 32 << 20; // 32MB likely larger than the kernel buffer

struct Conn {
    int fd = -1;
    // application's intention for the event loop
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    // buffered input/output data
    std::vector<uint8_t> incoming; // data to be parsed by the application
    std::vector<uint8_t> outgoing; // data to be sent to the peer
};

// append to the back
static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

// remove from the front
static void buf_consume(std::vector<uint8_t> &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);
}

// application callback when the listenning socket is ready 
static Conn *handle_accept(int fd) {
    // accept
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr*)&client_addr, &addrlen);

    if (connfd < 0) {
        msg_errno("accept failed");
        return NULL;
    }

    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
        ip & 0xFF,
        (ip >> 8) & 0xFF,
        (ip >> 16) & 0xFF,
        ip >> 24,
        ntohs(client_addr.sin_port)
    );

    // set the new connection fd to non-blocking mode
    fd_set_nb(connfd);

    // create a Conn object to represent this connection
    Conn *conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true; // set client connection to read mode
    return conn;
}

// process one request from the client connection if there is enough data
static bool try_one_request(Conn *conn) {
    // need at least 4 bytes header
    if (conn->incoming.size() < 4) {
        return false; // want read more data
    }

    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4); // assume little-endian

    if (len > k_max_msg) {
        msg("message too long");
        conn->want_close = true;
        return false; // will close the connection
    }

    // message body
    if (conn->incoming.size() < 4 + len) {
        return false;
    }
    const uint8_t *request = &conn->incoming[4];

    // got one request, do some application logic
    printf("client says: len:%d data:%.*s\n", len, len < 100 ? len : 100, request);

    // generate the response (echo)
    buf_append(conn->outgoing, (const uint8_t*)&len, 4); // header
    buf_append(conn->outgoing, request, len); // body

    // application logic done! remove the request message
    buf_consume(conn->incoming, 4 + len);

    return true; // success
}

// application callback when the socket is writable
static void handle_write(Conn *conn) {
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, &conn->outgoing[0], conn->outgoing.size());

    if (rv < 0 && errno == EAGAIN) {
        return; // actually not ready 
    }

    if (rv < 0) {
        msg_errno("write error");
        conn->want_close = true;
        return;
    }

    // remove written data from the `outgoing`
    buf_consume(conn->outgoing, (size_t)rv);

    // update the readiness intention 
    if (conn->outgoing.size() == 0) {
        conn->want_write = false; // no more data to write
        conn->want_read = true;  // switch to read mode
    } // else still want write
}

// application callback when the socket is readable
static void handle_read(Conn *conn) {
    uint8_t buf[64 * 1024]; // 64KB temporary buffer
    ssize_t rv = read(conn->fd, buf, sizeof(buf));

    if (rv < 0 && errno == EAGAIN) {
        return; // actually not ready 
    }

    if (rv < 0) {
        msg_errno("read error");
        conn->want_close = true;
        return;
    }

    // handle EOF
    if (rv == 0) {

        if (conn->incoming.size() == 0) {
            msg("client closed");
        } else {
            msg("unexpected EOF");
        }

        conn->want_close = true;
        return;
    }

    // got some new data
    buf_append(conn->incoming, buf, (size_t)rv);

    // parse requests and generate responses
    while (try_one_request(conn)) {}

    // update the readiness intention
    if (conn->outgoing.size() > 0) {
        conn->want_write = true; // have data to write
        conn->want_read = false; // switch to write mode
        // The socket is likely ready to write in a request-response protoco,
        // try to write immediately
        return handle_write(conn);
    } // else still want read
}

int main() {
    // the listening socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        die("socket failed");
    }

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // bind
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234); // Use htons to convert port number to network byte order
    addr.sin_addr.s_addr = htonl(0); // wildcard IP 0.0.0.0
    int rv = bind(fd, (const struct sockaddr*)&addr, sizeof(addr));

    if (rv) {
        die("bind failed");
    }

    // set the listen fd to non-blocking mode
    fd_set_nb(fd);

    // liseten
    rv = listen(fd, SOMAXCONN);

    if (rv) {
        die("listen failed");
    }

    // a map of all client connections keyed by the fd
    std::vector<Conn*> fd2conn;

    // the event loop
    std::vector<struct pollfd> poll_args;

    while (true) {
        // pepare the arguments for poll()
        poll_args.clear();
        // put the listening socket first
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);
        
        // the rest are connection sockets
        for (Conn *conn : fd2conn) {
            /* TODO: continue here */
        }
    }

    return 0;
}