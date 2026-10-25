#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "iotd.h"

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int net_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE("socket: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 4) < 0 || set_nonblock(fd) < 0) {
        LOGE("listen on port %d: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int net_accept(int listen_fd)
{
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int fd = accept(listen_fd, (struct sockaddr *)&sa, &sl);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    if (set_nonblock(fd) < 0) {
        close(fd);
        return -1;
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof(ip));
    LOGD("accepted %s:%u on fd %d", ip, ntohs(sa.sin_port), fd);
    return fd;
}

void client_init(struct client *c, size_t cap)
{
    c->fd = -1;
    c->buf = malloc(cap);
    c->cap = c->buf ? cap : 0;
    c->len = 0;
    c->dropped = 0;
    c->connected_at = 0;
}

void client_close(struct client *c)
{
    if (c->fd >= 0) {
        close(c->fd);
    }
    c->fd = -1;
    c->len = 0;
}

int client_queue(struct client *c, const uint8_t *data, size_t n, bool drop_lines)
{
    if (c->fd < 0) {
        return -1;
    }
    if (n > c->cap) {
        return -1;
    }
    if (c->len + n > c->cap) {
        if (!drop_lines) {
            LOGW("client fd %d: output buffer full, closing", c->fd);
            return -1;
        }
        /* Discard whole records from the front until the new one fits. */
        size_t cut = 0;
        while (c->len - cut + n > c->cap) {
            uint8_t *nl = memchr(c->buf + cut, '\n', c->len - cut);
            if (!nl) {
                cut = c->len;
                break;
            }
            cut = (size_t)(nl - c->buf) + 1;
            c->dropped++;
        }
        memmove(c->buf, c->buf + cut, c->len - cut);
        c->len -= cut;
    }
    memcpy(c->buf + c->len, data, n);
    c->len += n;
    return 0;
}

int client_flush(struct client *c)
{
    while (c->fd >= 0 && c->len > 0) {
        ssize_t w = send(c->fd, c->buf, c->len, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return 0;
            }
            LOGD("client fd %d: send: %s", c->fd, strerror(errno));
            return -1;
        }
        memmove(c->buf, c->buf + w, c->len - (size_t)w);
        c->len -= (size_t)w;
    }
    return 0;
}

void pollset_reset(struct pollset *ps) { ps->n = 0; }

int pollset_add(struct pollset *ps, int fd, short events)
{
    if (fd < 0 || ps->n >= POLLSET_MAX) {
        return -1;
    }
    ps->fds[ps->n].fd = fd;
    ps->fds[ps->n].events = events;
    ps->fds[ps->n].revents = 0;
    return ps->n++;
}

short pollset_revents(const struct pollset *ps, int idx)
{
    return idx < 0 ? 0 : ps->fds[idx].revents;
}
