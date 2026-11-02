/* Thread: the radio runs the OpenThread RCP image (no cpcd), and its UART is relayed byte for byte to one
 * TCP host, a border router (otbr-agent) speaking Spinel in HDLC-lite framing. The framing is self-
 * delimiting and otbr-agent resets the RCP itself when it starts, so the relay needs no protocol of its own.
 * Radio bytes that arrive while no host is connected are discarded. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "iotd.h"

#define CLIENT_BUF   (64 * 1024)
#define UART_BUF     (16 * 1024)
#define REOPEN_MS    1000

static struct {
    char uart[64];
    int uart_fd, uart_idx;
    uint8_t out[UART_BUF];   /* host bytes waiting for the UART */
    size_t out_len;
    uint64_t reopen_at;

    int listen_fd, listen_idx;
    struct allow allow;
    struct uplink up;        /* thread_connect; its fd moves to cl when the host speaks */
    int up_idx;
    struct client cl;
    int cl_idx;
    bool cl_is_uplink;

    unsigned long to_host, to_radio;
} T;

static int uart_open(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        LOGE("thread: open %s: %s", path, strerror(errno));
        return -1;
    }
    struct termios t;
    if (tcgetattr(fd, &t) < 0) {
        LOGE("thread: tcgetattr %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD | CRTSCTS; /* the RCP image uses hardware flow control */
    t.c_cflag &= ~CSTOPB;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        LOGE("thread: tcsetattr %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static void uart_close(uint64_t now, const char *why)
{
    if (T.uart_fd >= 0) {
        LOGW("thread: %s closed: %s, reopening", T.uart, why);
        close(T.uart_fd);
        T.uart_fd = -1;
    }
    T.out_len = 0;
    T.reopen_at = now + REOPEN_MS;
}

static void client_drop(const char *why)
{
    if (T.cl.fd >= 0) {
        LOGI("thread: client fd %d closed: %s (%lu bytes to the host, %lu to the radio)", T.cl.fd, why, T.to_host,
             T.to_radio);
        client_close(&T.cl);
        if (T.cl_is_uplink) {
            T.cl_is_uplink = false;
            T.up.next_try = now_ms() + 1000; /* the next host gets a fresh stream */
        }
    }
    T.out_len = 0;
}

static void client_accept(int fd, uint64_t now)
{
    if (T.cl.fd >= 0) {
        LOGW("thread: refusing second client (fd %d) while fd %d is connected", fd, T.cl.fd);
        close(fd);
        return;
    }
    T.cl.fd = fd;
    T.cl.len = 0;
    T.cl.connected_at = now;
    T.to_host = T.to_radio = 0;
    T.out_len = 0;
    if (T.uart_fd >= 0) {
        tcflush(T.uart_fd, TCIFLUSH); /* nothing the radio said before this host belongs to it */
    }
    LOGI("thread: client connected (fd %d)", fd);
}

static void client_read(void)
{
    if (T.out_len >= sizeof(T.out)) {
        return; /* the UART is behind; POLLIN stays off until it drains */
    }
    ssize_t r = read(T.cl.fd, T.out + T.out_len, sizeof(T.out) - T.out_len);
    if (r == 0) {
        client_drop("disconnected");
    } else if (r < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            client_drop(strerror(errno));
        }
    } else {
        T.out_len += (size_t)r;
        T.to_radio += (unsigned long)r;
    }
}

static void uart_write(uint64_t now)
{
    while (T.out_len > 0) {
        ssize_t w = write(T.uart_fd, T.out, T.out_len);
        if (w < 0) {
            if (errno != EAGAIN && errno != EINTR) {
                uart_close(now, strerror(errno));
            }
            return;
        }
        memmove(T.out, T.out + w, T.out_len - (size_t)w);
        T.out_len -= (size_t)w;
    }
}

static void uart_read(uint64_t now)
{
    uint8_t buf[2048];
    ssize_t r = read(T.uart_fd, buf, sizeof(buf));
    if (r < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            uart_close(now, strerror(errno));
        }
        return;
    }
    if (r == 0 || T.cl.fd < 0) {
        return;
    }
    T.to_host += (unsigned long)r;
    if (client_queue(&T.cl, buf, (size_t)r, false) < 0) {
        client_drop("host not reading");
    }
}

static bool uplink_has_data(int fd)
{
    uint8_t b;
    return recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT) > 0;
}

/* ---- module interface ---- */
int thread_init(const struct config *c)
{
    memset(&T, 0, sizeof(T));
    snprintf(T.uart, sizeof(T.uart), "%s", c->thread_uart);
    client_init(&T.cl, CLIENT_BUF);
    T.uart_idx = T.listen_idx = T.cl_idx = T.up_idx = -1;
    T.allow = c->thread_allow;
    uplink_init(&T.up, 256, c->thread_connect, "thread");
    T.uart_fd = uart_open(T.uart);
    if (T.uart_fd < 0) {
        return -1;
    }
    T.listen_fd = -1;
    if (c->thread_port > 0) {
        T.listen_fd = net_listen(c->thread_port);
        if (T.listen_fd < 0) {
            return -1;
        }
        LOGI("thread: Spinel/HDLC relay of %s on TCP port %d%s", T.uart, c->thread_port,
             T.allow.n ? " (allowed addresses only)" : "");
    }
    if (T.up.to[0]) {
        LOGI("thread: Spinel/HDLC host through %s", T.up.to);
    } else if (T.listen_fd < 0) {
        LOGE("thread: thread_port is 0 and there is no thread_connect: no way for a host to reach the radio");
        return -1;
    }
    return 0;
}

void thread_tick(uint64_t now)
{
    if (T.uart_fd < 0 && now >= T.reopen_at) {
        T.uart_fd = uart_open(T.uart);
        if (T.uart_fd < 0) {
            T.reopen_at = now + REOPEN_MS;
        } else {
            LOGI("thread: %s open again", T.uart);
        }
    }
    if (T.cl.fd < 0) {
        uplink_tick(&T.up, now); /* while a host is connected, no second stream */
    }
}

void thread_poll_add(struct pollset *ps)
{
    T.listen_idx = pollset_add(ps, T.listen_fd, POLLIN);
    T.up_idx = T.up.c.fd >= 0 ? pollset_add(ps, T.up.c.fd, uplink_events(&T.up)) : -1;
    T.cl_idx = T.cl.fd >= 0
                   ? pollset_add(ps, T.cl.fd, (T.out_len < sizeof(T.out) ? POLLIN : 0) | (T.cl.len ? POLLOUT : 0))
                   : -1;
    T.uart_idx = T.uart_fd >= 0 ? pollset_add(ps, T.uart_fd, POLLIN | (T.out_len ? POLLOUT : 0)) : -1;
}

void thread_poll_handle(const struct pollset *ps, uint64_t now)
{
    if (pollset_revents(ps, T.listen_idx) & POLLIN) {
        uint32_t peer;
        int fd = net_accept(T.listen_fd, &peer);
        if (fd >= 0 && !allow_match(&T.allow, peer)) {
            char ip[16];
            const uint8_t *p = (const uint8_t *)&peer;
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
            LOGW("thread: refusing %s: not in thread_allow", ip);
            close(fd);
        } else if (fd >= 0) {
            client_accept(fd, now);
        }
    }
    short ure = pollset_revents(ps, T.up_idx);
    if (T.up.c.fd >= 0 && ure) {
        if (T.up.connecting) {
            uplink_connected(&T.up, ure, now);
        } else if ((ure & POLLOUT) && T.up.c.len && client_flush(&T.up.c) < 0) {
            uplink_close(&T.up, now, "closed", false);
        } else if (ure & (POLLERR | POLLHUP)) {
            uplink_close(&T.up, now, "closed by the other end", false);
        } else if ((ure & POLLIN) && !uplink_has_data(T.up.c.fd)) {
            uplink_close(&T.up, now, "closed by the other end", false);
        } else if (ure & POLLIN) {
            /* The host spoke: the stream becomes the client (its bytes are read next round). */
            int fd = T.up.c.fd;
            T.up.c.fd = -1;
            T.up.c.len = 0;
            if (T.cl.fd >= 0) {
                LOGW("thread: host on the stream while a listener client is connected, closing the stream");
                close(fd);
                T.up.next_try = now + 1000;
            } else {
                LOGI("thread: host speaking through %s", T.up.to);
                client_accept(fd, now);
                T.cl_is_uplink = true;
            }
        }
    }
    short re = pollset_revents(ps, T.cl_idx);
    if (T.cl.fd >= 0 && (re & (POLLIN | POLLERR | POLLHUP))) {
        client_read();
    }
    if (T.cl.fd >= 0 && (re & POLLOUT) && client_flush(&T.cl) < 0) {
        client_drop("write error");
    }
    short ue = pollset_revents(ps, T.uart_idx);
    if (T.uart_fd >= 0 && (ue & POLLIN)) {
        uart_read(now);
    }
    if (T.uart_fd >= 0 && (ue & (POLLERR | POLLHUP))) {
        uart_close(now, "error");
    }
    if (T.uart_fd >= 0 && T.out_len) {
        uart_write(now);
    }
}

void thread_shutdown(void)
{
    client_drop("shutdown");
    client_close(&T.up.c);
    if (T.listen_fd >= 0) {
        close(T.listen_fd);
        T.listen_fd = -1;
    }
    if (T.uart_fd >= 0) {
        close(T.uart_fd);
        T.uart_fd = -1;
    }
}
