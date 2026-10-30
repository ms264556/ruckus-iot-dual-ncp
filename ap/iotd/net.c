#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
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

int net_accept(int listen_fd, uint32_t *peer)
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
    if (peer) {
        *peer = sa.sin_addr.s_addr;
    }
    return fd;
}

bool net_parse_ip_port(const char *ip_port, uint32_t *addr, int *port)
{
    char ip[INET_ADDRSTRLEN];
    const char *colon = strrchr(ip_port, ':');
    if (!colon || (size_t)(colon - ip_port) >= sizeof(ip)) {
        return false;
    }
    memcpy(ip, ip_port, (size_t)(colon - ip_port));
    ip[colon - ip_port] = 0;
    char *end;
    long p = strtol(colon + 1, &end, 10);
    if (*end || p < 1 || p > 65535 || inet_pton(AF_INET, ip, addr) != 1) {
        return false;
    }
    *port = (int)p;
    return true;
}

int net_connect(const char *ip_port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    int port;
    if (!net_parse_ip_port(ip_port, &sa.sin_addr.s_addr, &port)) {
        return -1;
    }
    sa.sin_port = htons((uint16_t)port);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE("socket: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    if (set_nonblock(fd) < 0 || (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS)) {
        LOGD("connect %s: %s", ip_port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int net_connect_result(int fd)
{
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return errno;
    }
    return err;
}

bool allow_parse(struct allow *a, const char *list)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", list);
    a->n = 0;
    for (char *tok = strtok(buf, " ,\t"); tok; tok = strtok(NULL, " ,\t")) {
        int len = 32;
        char *slash = strchr(tok, '/');
        if (slash) {
            char *end;
            long l = strtol(slash + 1, &end, 10);
            if (*end || l < 0 || l > 32) {
                return false;
            }
            len = (int)l;
            *slash = 0;
        }
        struct in_addr in;
        if (a->n >= ALLOW_MAX || inet_pton(AF_INET, tok, &in) != 1) {
            return false;
        }
        uint32_t mask = len ? htonl(0xFFFFFFFFu << (32 - len)) : 0;
        a->net[a->n] = in.s_addr & mask;
        a->mask[a->n] = mask;
        a->n++;
    }
    return true;
}

bool allow_match(const struct allow *a, uint32_t addr)
{
    if (a->n == 0) {
        return true;
    }
    for (int i = 0; i < a->n; i++) {
        if ((addr & a->mask[i]) == a->net[i]) {
            return true;
        }
    }
    return false;
}

#define UPLINK_MIN_BACKOFF_MS  1000
#define UPLINK_MAX_BACKOFF_MS  30000
#define UPLINK_STABLE_MS       30000

void uplink_init(struct uplink *u, size_t cap, const char *to, const char *tag)
{
    client_init(&u->c, cap);
    snprintf(u->to, sizeof(u->to), "%s", to);
    u->tag = tag;
    u->connecting = false;
    u->next_try = 0;
    u->backoff = UPLINK_MIN_BACKOFF_MS;
    u->last_error = 0;
}

bool uplink_up(const struct uplink *u) { return u->c.fd >= 0 && !u->connecting; }

void uplink_tick(struct uplink *u, uint64_t now)
{
    if (!u->to[0] || u->c.fd >= 0 || now < u->next_try) {
        return;
    }
    int fd = net_connect(u->to);
    if (fd < 0) {
        u->next_try = now + u->backoff;
        return;
    }
    u->c.fd = fd;
    u->c.len = 0;
    u->c.dropped = 0;
    u->c.connected_at = now;
    u->connecting = true;
}

short uplink_events(const struct uplink *u)
{
    return u->connecting ? POLLOUT : (short)(POLLIN | (u->c.len ? POLLOUT : 0));
}

void uplink_close(struct uplink *u, uint64_t now, const char *why, bool again_soon)
{
    if (u->c.fd < 0) {
        return;
    }
    bool was_up = !u->connecting;
    if (again_soon || (was_up && now - u->c.connected_at >= UPLINK_STABLE_MS)) {
        u->backoff = UPLINK_MIN_BACKOFF_MS;
    }
    if (was_up) {
        LOGI("%s: stream to %s %s (%u records dropped while connected)", u->tag, u->to, why, u->c.dropped);
    }
    client_close(&u->c);
    u->connecting = false;
    u->next_try = now + u->backoff;
    if (!again_soon) {
        u->backoff = u->backoff * 2 > UPLINK_MAX_BACKOFF_MS ? UPLINK_MAX_BACKOFF_MS : u->backoff * 2;
    }
}

bool uplink_connected(struct uplink *u, short re, uint64_t now)
{
    if (!u->connecting || !re) {
        return false;
    }
    int err = net_connect_result(u->c.fd);
    if (err) {
        if (err != u->last_error) {
            LOGI("%s: connect to %s: %s, retrying", u->tag, u->to, strerror(err));
        }
        u->last_error = err;
        uplink_close(u, now, "failed", false);
        return false;
    }
    u->connecting = false;
    u->c.connected_at = now;
    u->last_error = 0;
    char hello[32];
    int n = snprintf(hello, sizeof(hello), "iotd %s 1\n", u->tag);
    if (client_queue(&u->c, (const uint8_t *)hello, (size_t)n, false) < 0 || client_flush(&u->c) < 0) {
        uplink_close(u, now, "closed", false);
        return false;
    }
    LOGI("%s: stream to %s connected", u->tag, u->to);
    return true;
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
