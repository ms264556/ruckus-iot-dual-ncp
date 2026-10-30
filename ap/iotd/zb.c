/* Zigbee: the NCP side of ASH over TCP, relaying EZSP frames to CPC endpoint 5.
 *
 * Home Assistant (bellows) or zigbee2mqtt connects to the TCP port exactly as it
 * would to ser2net in front of a serial EZSP coordinator. We terminate ASH here:
 *   host RST                 -> RSTACK (version 2, reset code 0x0B "software")
 *   host DATA(frm, ack)      -> ACK, EZSP payload (de-randomized) to endpoint 5
 *   NCP EZSP frame           -> DATA(frm, ack) to the host, retransmitted on NAK
 *                               or ack timeout, ERROR after too many timeouts
 * Over CPC the EZSP frames are raw: no ASH, no length prefix, one frame per read.
 *
 * The host is either a client of the TCP listener (zigbee_port, limited to zigbee_allow) or
 * whoever is behind the outbound stream (zigbee_connect): iotd keeps that connection open
 * idle, and the first byte from it makes it the client, as an accept would.
 *
 * One client at a time. Endpoint 5 is opened when a client connects and closed
 * when it leaves, so the next client finds the NCP's EZSP layer in the same state
 * the SDK's own CPC host (serial-interface-cpc.c) leaves it in. The Zigbee stack
 * itself is not reset by an ASH RST: the network stays up across reconnects.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ash_codec.h"
#include "iotd.h"

#define TX_WINDOW        3      /* unacked DATA frames to the host */
#define ACK_TIMEOUT_MS   800
#define MAX_TIMEOUTS     6
#define EP_RETRY_MS      250
#define EP_RETRY_LOG_AT  8      /* log once this many retries have failed (2 s) */
#define TXQ_LEN          64     /* NCP frames waiting for the window */
#define NCPQ_LEN         16     /* host frames waiting for the endpoint to open */
#define CLIENT_BUF       (64 * 1024)

struct frame {
    uint8_t len;
    uint8_t data[ASH_MAX_DATA];
};

struct ring {
    struct frame f[TXQ_LEN];
    int head, count, cap;
};

static struct {
    int listen_fd, listen_idx;
    struct allow allow;
    struct uplink up;        /* zigbee_connect; its fd moves to cl when the host speaks */
    int up_idx;
    struct client cl;
    int cl_idx;
    bool cl_is_uplink;
    struct ash_decoder dec;

    cpc_endpoint_t ep;
    int ep_fd, ep_idx;
    enum { EP_CLOSED, EP_OPENING, EP_OPEN } ep_state;
    unsigned ep_generation;
    uint64_t ep_next_try;
    int ep_tries;

    /* NCP reset: cpcd is restarted and the endpoint reopened on the new link */
    bool reset_pending;      /* waiting for the new cpcd and endpoint 5 */
    bool rst_waiting;        /* host sent RST, RSTACK goes out when the NCP is back */
    bool ncp_fresh;          /* reset done and no EZSP frame sent to the NCP since */
    uint64_t reset_started;
    unsigned reset_generation;

    /* ASH link state (NCP side) */
    bool connected;
    uint8_t frm_tx;   /* number of the next DATA frame we send */
    uint8_t ack_rx;   /* oldest frame the host has not acknowledged */
    uint8_t frm_rx;   /* number we expect in the next host DATA frame */
    bool rejecting;   /* NAK sent, waiting for the host to retransmit */
    struct frame retx[8];
    uint64_t ack_deadline;
    int timeouts;
    struct ring txq;    /* from NCP, waiting for the window */
    struct ring ncpq;   /* from host, waiting for the endpoint */

    unsigned long host_frames, ncp_frames, retransmits, naks_sent, bad_frames;
    uint8_t rx[SL_CPC_READ_MINIMUM_SIZE];
} Z;

/* ---- rings ---- */
static void ring_init(struct ring *r, int cap) { r->head = 0; r->count = 0; r->cap = cap; }
static bool ring_push(struct ring *r, const uint8_t *d, size_t n)
{
    if (n > ASH_MAX_DATA) {
        return false;
    }
    if (r->count == r->cap) { /* drop oldest */
        r->head = (r->head + 1) % r->cap;
        r->count--;
    }
    struct frame *f = &r->f[(r->head + r->count) % r->cap];
    f->len = (uint8_t)n;
    memcpy(f->data, d, n);
    r->count++;
    return true;
}
static struct frame *ring_peek(struct ring *r) { return r->count ? &r->f[r->head] : NULL; }
static void ring_pop(struct ring *r)
{
    if (r->count) {
        r->head = (r->head + 1) % r->cap;
        r->count--;
    }
}

/* ---- output to the host ---- */
static void send_frame(const uint8_t *frame, size_t n)
{
    uint8_t enc[ASH_MAX_ENCODED];
    size_t e = ash_encode(frame, n, enc, sizeof(enc));
    if (!e) {
        LOGE("zb: cannot encode %zu-byte frame", n);
        return;
    }
    log_hex(LVL_DEBUG, "zb: ash tx", frame, n);
    if (client_queue(&Z.cl, enc, e, false) < 0 || client_flush(&Z.cl) < 0) {
        LOGW("zb: client write failed");
        /* closed by the poll handler on the next pass */
    }
}

static void send_control(uint8_t control)
{
    send_frame(&control, 1);
}

static void send_ack(void)
{
    send_control((uint8_t)(ASH_CONTROL_ACK | Z.frm_rx));
}

static void send_nak(void)
{
    Z.naks_sent++;
    send_control((uint8_t)(ASH_CONTROL_NAK | Z.frm_rx));
}

static void send_rstack(void)
{
    uint8_t f[3] = { ASH_CONTROL_RSTACK, ASH_VERSION, ASH_RESET_SOFTWARE };
    send_frame(f, sizeof(f));
}

static void send_error(uint8_t code)
{
    uint8_t f[3] = { ASH_CONTROL_ERROR, ASH_VERSION, code };
    send_frame(f, sizeof(f));
}

static void send_data(uint8_t frm, bool retx, const struct frame *f, uint64_t now)
{
    uint8_t buf[1 + ASH_MAX_DATA];
    buf[0] = (uint8_t)((frm << 4) | (retx ? 0x08 : 0) | Z.frm_rx);
    memcpy(buf + 1, f->data, f->len);
    ash_randomize(buf + 1, f->len);
    send_frame(buf, 1 + f->len);
    if (Z.ack_deadline == 0) {
        Z.ack_deadline = now + ACK_TIMEOUT_MS;
    }
}

static int window_used(void) { return (Z.frm_tx - Z.ack_rx) & 7; }

static void send_pending(uint64_t now)
{
    struct frame *f;
    while (Z.connected && window_used() < TX_WINDOW && (f = ring_peek(&Z.txq))) {
        Z.retx[Z.frm_tx] = *f;
        ring_pop(&Z.txq);
        send_data(Z.frm_tx, false, &Z.retx[Z.frm_tx], now);
        Z.frm_tx = (Z.frm_tx + 1) & 7;
    }
}

static void retransmit(uint64_t now)
{
    Z.ack_deadline = 0;
    for (uint8_t f = Z.ack_rx; f != Z.frm_tx; f = (f + 1) & 7) {
        Z.retransmits++;
        send_data(f, true, &Z.retx[f], now);
    }
}

static void link_reset(void)
{
    Z.connected = false;
    Z.frm_tx = Z.ack_rx = Z.frm_rx = 0;
    Z.rejecting = false;
    Z.ack_deadline = 0;
    Z.timeouts = 0;
    ring_init(&Z.txq, TXQ_LEN);
    ring_init(&Z.ncpq, NCPQ_LEN);
    ash_decoder_reset(&Z.dec);
}

/* ---- endpoint 5 ---- */
static void ep_close(void)
{
    if (Z.ep_state == EP_OPEN) {
        cpc_close_endpoint(&Z.ep);
        LOGI("zb: endpoint %d closed", IOTD_ZB_ENDPOINT);
    }
    Z.ep_state = EP_CLOSED;
    Z.ep_fd = -1;
}

static void ep_begin_open(uint64_t now)
{
    Z.ep_state = EP_OPENING;
    Z.ep_next_try = now;
    Z.ep_tries = 0;
}

/* Reboot the NCP through a cpcd restart. The endpoint is reopened once the link
 * has come back on a new cpcd (link generation changes). */
static void ncp_reset_begin(uint64_t now, const char *why)
{
    ep_close();
    Z.reset_pending = true;
    Z.ncp_fresh = false;
    Z.reset_started = now;
    Z.reset_generation = link_generation();
    ep_begin_open(now);
    cpcd_restart(why);
}

static void ncp_write(const uint8_t *d, size_t n)
{
    Z.ncp_fresh = false;
    log_hex(LVL_DEBUG, "zb: ezsp to ncp", d, n);
    ssize_t w = cpc_write_endpoint(Z.ep, d, n, CPC_ENDPOINT_WRITE_FLAG_NONE);
    if (w != (ssize_t)n) {
        LOGE("zb: cpc_write_endpoint: %zd", w);
        link_request_down("zb write failed");
    }
}

static void ep_try_open(uint64_t now)
{
    if (Z.ep_state != EP_OPENING || !link_up() || now < Z.ep_next_try) {
        return;
    }
    if (Z.reset_pending && link_generation() == Z.reset_generation) {
        return; /* still the old cpcd; wait for the restart */
    }
    int fd = cpc_open_endpoint(link_handle(), &Z.ep, IOTD_ZB_ENDPOINT, 1);
    if (fd < 0) {
        Z.ep_tries++;
        if (Z.ep_tries == EP_RETRY_LOG_AT) {
            LOGI("zb: endpoint %d not yet open on the NCP (%d), still retrying", IOTD_ZB_ENDPOINT, fd);
        }
        Z.ep_next_try = now + EP_RETRY_MS;
        return;
    }
    Z.ep_fd = fd;
    Z.ep_state = EP_OPEN;
    Z.ep_generation = link_generation();
    LOGI("zb: endpoint %d open (fd %d) after %d retries", IOTD_ZB_ENDPOINT, fd, Z.ep_tries);
    if (Z.reset_pending) {
        Z.reset_pending = false;
        Z.ncp_fresh = true;
        LOGI("zb: NCP reset complete in %llu ms%s", (unsigned long long)(now - Z.reset_started),
             Z.rst_waiting ? ", sending RSTACK" : "");
        if (Z.rst_waiting) {
            Z.rst_waiting = false;
            send_rstack();
        }
    }
    struct frame *f;
    while ((f = ring_peek(&Z.ncpq))) {
        ncp_write(f->data, f->len);
        ring_pop(&Z.ncpq);
    }
}

static void ep_read(uint64_t now)
{
    for (int burst = 0; burst < 32; burst++) {
        ssize_t n = cpc_read_endpoint(Z.ep, Z.rx, sizeof(Z.rx), CPC_ENDPOINT_READ_FLAG_NON_BLOCKING);
        if (n < 0) {
            if (-n == EAGAIN || -n == EWOULDBLOCK) {
                return;
            }
            log_msg(cpcd_restarting() ? LVL_INFO : LVL_ERR, "zb: cpc_read_endpoint: %zd%s", n,
                    cpcd_restarting() ? " (cpcd restart in progress)" : "");
            link_request_down(cpcd_restarting() ? "cpcd restarting" : "zb read failed");
            return;
        }
        if (n == 0) {
            return;
        }
        Z.ncp_frames++;
        log_hex(LVL_DEBUG, "zb: ezsp from ncp", Z.rx, (size_t)n);
        if ((size_t)n > ASH_MAX_DATA) {
            LOGW("zb: %zd-byte EZSP frame from NCP dropped (limit %d)", n, ASH_MAX_DATA);
            continue;
        }
        if (Z.txq.count == Z.txq.cap) {
            LOGW("zb: host is not acknowledging, oldest queued NCP frame dropped");
        }
        ring_push(&Z.txq, Z.rx, (size_t)n);
        send_pending(now);
    }
}

/* ---- client ---- */
/* Bytes are waiting on the idle stream; false at EOF or on an error. */
static bool uplink_has_data(int fd)
{
    uint8_t b;
    return recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT) > 0;
}

static void client_drop(const char *why)
{
    if (Z.cl.fd >= 0) {
        LOGI("zb: client fd %d closed: %s (%lu host frames, %lu ncp frames, %lu retransmits, %lu naks)", Z.cl.fd,
             why, Z.host_frames, Z.ncp_frames, Z.retransmits, Z.naks_sent);
        client_close(&Z.cl);
        if (Z.cl_is_uplink) {
            Z.cl_is_uplink = false;
            Z.up.next_try = now_ms() + 1000; /* the next host gets a fresh stream */
        }
    }
    ep_close();
    link_reset();
    Z.reset_pending = Z.rst_waiting = false;
}

static void client_accept(int fd, uint64_t now)
{
    if (Z.cl.fd >= 0) {
        LOGW("zb: refusing second client (fd %d) while fd %d is connected", fd, Z.cl.fd);
        close(fd);
        return;
    }
    Z.cl.fd = fd;
    Z.cl.len = 0;
    Z.cl.connected_at = now;
    Z.host_frames = Z.ncp_frames = Z.retransmits = Z.naks_sent = Z.bad_frames = 0;
    link_reset();
    LOGI("zb: client connected (fd %d), resetting the NCP", fd);
    /* A serial coordinator reboots on the host's RST and comes up with no endpoints
     * registered and the stack down. Give this host the same, before its RST arrives
     * so the reply is quick. */
    ncp_reset_begin(now, "Zigbee client connected");
}

/* ackNum from the host acknowledges every frame before it */
static void process_ack(uint8_t ack, uint64_t now)
{
    uint8_t span = (Z.frm_tx - Z.ack_rx) & 7;
    uint8_t dist = (ack - Z.ack_rx) & 7;
    if (dist > span) {
        LOGD("zb: ignoring ackNum %u outside [%u, %u]", ack, Z.ack_rx, Z.frm_tx);
        return;
    }
    if (dist) {
        Z.ack_rx = ack;
        Z.timeouts = 0;
        Z.ack_deadline = window_used() ? now + ACK_TIMEOUT_MS : 0;
    }
    send_pending(now);
}

static void host_frame(const uint8_t *f, size_t n, uint64_t now)
{
    uint8_t c = f[0];
    log_hex(LVL_DEBUG, "zb: ash rx", f, n);
    if (c == ASH_CONTROL_RST) {
        link_reset();
        Z.connected = true;
        if (Z.reset_pending) {
            LOGI("zb: RST from host while the NCP reset is in progress, RSTACK follows");
            Z.rst_waiting = true;
        } else if (Z.ncp_fresh) {
            LOGI("zb: RST from host, NCP is freshly reset, replying RSTACK");
            send_rstack();
        } else {
            LOGI("zb: RST from host, resetting the NCP, RSTACK follows");
            Z.rst_waiting = true;
            ncp_reset_begin(now, "RST from Zigbee host");
        }
        return;
    }
    if (!(c & 0x80)) { /* DATA */
        uint8_t frm = (c >> 4) & 7, ack = c & 7;
        bool retx = c & 0x08;
        if (!Z.connected) {
            LOGD("zb: DATA before RST, accepting anyway");
            Z.connected = true;
        }
        bool in_seq = frm == Z.frm_rx;
        if (in_seq) {
            Z.frm_rx = (Z.frm_rx + 1) & 7; /* so DATA frames sent below carry the new ackNum */
        }
        process_ack(ack, now);
        if (in_seq) {
            Z.rejecting = false;
            Z.host_frames++;
            uint8_t ezsp[ASH_MAX_DATA];
            size_t len = n - 1;
            if (len == 0 || len > ASH_MAX_DATA) {
                LOGW("zb: DATA frame with %zu-byte payload ignored", len);
            } else {
                memcpy(ezsp, f + 1, len);
                ash_randomize(ezsp, len);
                if (Z.ep_state == EP_OPEN) {
                    ncp_write(ezsp, len);
                } else {
                    LOGD("zb: endpoint not open yet, queuing %zu-byte host frame", len);
                    ring_push(&Z.ncpq, ezsp, len);
                }
            }
            send_ack();
        } else if (retx) {
            send_ack(); /* duplicate of something we already have */
        } else if (!Z.rejecting) {
            LOGW("zb: out-of-sequence DATA frame %u (expected %u), sending NAK", frm, Z.frm_rx);
            Z.rejecting = true;
            send_nak();
        }
        return;
    }
    switch (c & 0xE0) {
    case ASH_CONTROL_ACK:
        process_ack(c & 7, now);
        break;
    case ASH_CONTROL_NAK:
        process_ack(c & 7, now);
        LOGD("zb: NAK %u from host, retransmitting %d frame(s)", c & 7, window_used());
        retransmit(now);
        break;
    default:
        LOGW("zb: unexpected control byte 0x%02x from host", c);
        break;
    }
}

static void client_read(uint64_t now)
{
    uint8_t buf[2048];
    ssize_t r = read(Z.cl.fd, buf, sizeof(buf));
    if (r == 0) {
        client_drop("disconnected");
        return;
    }
    if (r < 0) {
        if (errno == EAGAIN || errno == EINTR) {
            return;
        }
        client_drop(strerror(errno));
        return;
    }
    for (ssize_t i = 0; i < r; i++) {
        int d = ash_decode_byte(&Z.dec, buf[i]);
        if (d > 0) {
            host_frame(Z.dec.buf, (size_t)d, now);
        } else if (d == -1) {
            Z.bad_frames++;
            LOGW("zb: bad frame from host (CRC or length), sending NAK");
            if (!Z.rejecting) {
                Z.rejecting = true;
                send_nak();
            }
        }
        if (Z.cl.fd < 0) {
            return;
        }
    }
}

/* ---- module interface ---- */
int zb_init(const struct config *c)
{
    memset(&Z, 0, sizeof(Z));
    client_init(&Z.cl, CLIENT_BUF);
    Z.ep_fd = -1;
    Z.listen_idx = Z.cl_idx = Z.ep_idx = Z.up_idx = -1;
    link_reset();
    Z.allow = c->zigbee_allow;
    uplink_init(&Z.up, 256, c->zigbee_connect, "zigbee");
    Z.listen_fd = -1;
    if (c->zigbee_port > 0) {
        Z.listen_fd = net_listen(c->zigbee_port);
        if (Z.listen_fd < 0) {
            return -1;
        }
        LOGI("zb: EZSP/ASH server on TCP port %d%s", c->zigbee_port,
             Z.allow.n ? " (allowed addresses only)" : "");
    }
    if (Z.up.to[0]) {
        LOGI("zb: EZSP/ASH host through %s", Z.up.to);
    } else if (Z.listen_fd < 0) {
        LOGE("zb: zigbee_port is 0 and there is no zigbee_connect: no way for a host to reach the coordinator");
        return -1;
    }
    return 0;
}

void zb_link_down(void)
{
    if (Z.ep_state == EP_OPEN) {
        cpc_close_endpoint(&Z.ep);
        Z.ep_fd = -1;
    }
    Z.ep_state = EP_CLOSED;
    if (Z.cl.fd >= 0) {
        if (Z.reset_pending) {
            Z.ep_state = EP_OPENING; /* expected: cpcd is restarting for us */
        } else {
            /* The NCP context behind the endpoint is gone; make the host reconnect. */
            client_drop("cpc link lost");
        }
    }
}

void zb_tick(uint64_t now)
{
    if (Z.ep_state == EP_OPEN && Z.ep_generation != link_generation()) {
        zb_link_down();
    }
    if (Z.reset_pending && now - Z.reset_started > 10000) {
        client_drop("NCP reset did not complete within 10 s");
    }
    ep_try_open(now);
    if (Z.cl.fd < 0) {
        uplink_tick(&Z.up, now); /* while a host is connected, no second stream */
    }
    if (Z.ack_deadline && now >= Z.ack_deadline) {
        Z.timeouts++;
        if (Z.timeouts > MAX_TIMEOUTS) {
            LOGW("zb: host did not acknowledge after %d timeouts, sending ERROR", MAX_TIMEOUTS);
            send_error(ASH_ERROR_ACK_TIMEOUTS);
            link_reset(); /* ASH state only; the endpoint stays open and the host must RST */
        } else {
            LOGD("zb: ack timeout %d, retransmitting %d frame(s)", Z.timeouts, window_used());
            retransmit(now);
        }
    }
}

void zb_poll_add(struct pollset *ps)
{
    Z.listen_idx = pollset_add(ps, Z.listen_fd, POLLIN);
    Z.up_idx = Z.up.c.fd >= 0 ? pollset_add(ps, Z.up.c.fd, uplink_events(&Z.up)) : -1;
    Z.cl_idx = Z.cl.fd >= 0 ? pollset_add(ps, Z.cl.fd, POLLIN | (Z.cl.len ? POLLOUT : 0)) : -1;
    Z.ep_idx = Z.ep_state == EP_OPEN ? pollset_add(ps, Z.ep_fd, POLLIN) : -1;
}

void zb_poll_handle(const struct pollset *ps, uint64_t now)
{
    if (pollset_revents(ps, Z.listen_idx) & POLLIN) {
        uint32_t peer;
        int fd = net_accept(Z.listen_fd, &peer);
        if (fd >= 0 && !allow_match(&Z.allow, peer)) {
            char ip[16];
            const uint8_t *p = (const uint8_t *)&peer;
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
            LOGW("zb: refusing %s: not in zigbee_allow", ip);
            close(fd);
        } else if (fd >= 0) {
            client_accept(fd, now);
        }
    }
    short ure = pollset_revents(ps, Z.up_idx);
    if (Z.up.c.fd >= 0 && ure) {
        if (Z.up.connecting) {
            uplink_connected(&Z.up, ure, now);
        } else if ((ure & POLLOUT) && Z.up.c.len && client_flush(&Z.up.c) < 0) {
            uplink_close(&Z.up, now, "closed", false);
        } else if (ure & (POLLERR | POLLHUP)) {
            uplink_close(&Z.up, now, "closed by the other end", false);
        } else if ((ure & POLLIN) && !uplink_has_data(Z.up.c.fd)) {
            uplink_close(&Z.up, now, "closed by the other end", false);
        } else if (ure & POLLIN) {
            /* The host spoke: the stream becomes the client (its bytes are read next round). */
            int fd = Z.up.c.fd;
            Z.up.c.fd = -1;
            Z.up.c.len = 0;
            if (Z.cl.fd >= 0) {
                LOGW("zb: host on the stream while a listener client is connected, closing the stream");
                close(fd);
                Z.up.next_try = now + 1000;
            } else {
                LOGI("zb: host speaking through %s", Z.up.to);
                client_accept(fd, now);
                Z.cl_is_uplink = true;
            }
        }
    }
    short re = pollset_revents(ps, Z.cl_idx);
    if (Z.cl.fd >= 0 && (re & (POLLIN | POLLERR | POLLHUP))) {
        client_read(now);
    }
    if (Z.cl.fd >= 0 && (re & POLLOUT) && client_flush(&Z.cl) < 0) {
        client_drop("write error");
    }
    if (Z.ep_state == EP_OPEN && (pollset_revents(ps, Z.ep_idx) & (POLLIN | POLLERR | POLLHUP))) {
        ep_read(now);
    }
}

void zb_shutdown(void)
{
    client_drop("shutdown");
    client_close(&Z.up.c);
    if (Z.listen_fd >= 0) {
        close(Z.listen_fd);
        Z.listen_fd = -1;
    }
}
