/* Bluetooth: BGAPI over CPC endpoint 2, advertisements out as newline-delimited JSON.
 *
 * BGAPI framing (little-endian 4-byte header, then payload):
 *   byte 0: msg_type (0x00 command/response, 0x80 event) | device_type 0x20 | len bits 10..8
 *   byte 1: len bits 7..0
 *   byte 2: class
 *   byte 3: method
 * The ids below are the GSDK 4.4.0 / Bluetooth 7.0 values from sl_bt_api.h written as
 * the 32-bit little-endian header with a zero length.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "iotd.h"

#define BGAPI_ID_MASK                 0xFFFF00F8u
#define CMD_SYSTEM_HELLO              0x00010020u
#define CMD_SYSTEM_GET_VERSION        0x1B010020u
#define CMD_SCANNER_SET_PARAMETERS    0x06050020u
#define CMD_SCANNER_START             0x03050020u
#define CMD_SCANNER_STOP              0x05050020u
#define EVT_SYSTEM_BOOT               0x000100A0u
#define EVT_SYSTEM_ERROR              0x060100A0u
#define EVT_SCANNER_LEGACY_REPORT     0x000500A0u
#define EVT_SCANNER_EXTENDED_REPORT   0x020500A0u

/* Scan settings: active scanning so scan responses arrive, 1M PHY, observation
 * mode (every advertiser, not only discoverable ones), window == interval so the
 * scanner is always listening. Units of 0.625 ms. */
#define SCAN_MODE_ACTIVE      1
#define SCAN_INTERVAL         0x00A0  /* 100 ms */
#define SCAN_WINDOW           0x00A0
#define SCAN_PHY_1M           1
#define SCAN_DISCOVER_OBSERVE 2

#define MAX_CLIENTS      16
#define CLIENT_BUF       (128 * 1024)
#define EP_RETRY_MS      500
#define STATS_PERIOD_MS  60000

static struct {
    int listen_fd;
    int listen_idx;
    struct client clients[MAX_CLIENTS];
    int client_idx[MAX_CLIENTS];
    cpc_endpoint_t ep;
    int ep_fd;
    int ep_idx;
    bool ep_open;
    unsigned ep_generation;
    uint64_t ep_next_try;
    bool ep_failure_logged;
    uint64_t stats_at;
    unsigned long adverts, legacy, extended, boots, events_other, dropped;
    uint8_t rx[SL_CPC_READ_MINIMUM_SIZE];
} B;

static int clients_connected(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        n += B.clients[i].fd >= 0;
    }
    return n;
}

static int bgapi_send(uint32_t id, const uint8_t *payload, size_t len)
{
    uint8_t buf[4 + 64];
    if (len > 64) {
        return -1;
    }
    uint32_t hdr = id | ((uint32_t)(len & 0xFF) << 8) | ((uint32_t)(len >> 8) & 7);
    buf[0] = (uint8_t)hdr;
    buf[1] = (uint8_t)(hdr >> 8);
    buf[2] = (uint8_t)(hdr >> 16);
    buf[3] = (uint8_t)(hdr >> 24);
    memcpy(buf + 4, payload, len);
    ssize_t w = cpc_write_endpoint(B.ep, buf, 4 + len, CPC_ENDPOINT_WRITE_FLAG_NONE);
    if (w != (ssize_t)(4 + len)) {
        LOGE("ble: cpc_write_endpoint: %zd", w);
        link_request_down("ble write failed");
        return -1;
    }
    log_hex(LVL_DEBUG, "ble: tx", buf, 4 + len);
    return 0;
}

static void scan_setup(void)
{
    LOGI("ble: configuring scanner: active, 1M PHY, interval %u window %u (x0.625 ms), observation",
         SCAN_INTERVAL, SCAN_WINDOW);
    bgapi_send(CMD_SYSTEM_GET_VERSION, NULL, 0);
    bgapi_send(CMD_SCANNER_STOP, NULL, 0); /* harmless if not scanning */
    uint8_t p[5] = { SCAN_MODE_ACTIVE, SCAN_INTERVAL & 0xFF, SCAN_INTERVAL >> 8, SCAN_WINDOW & 0xFF, SCAN_WINDOW >> 8 };
    bgapi_send(CMD_SCANNER_SET_PARAMETERS, p, sizeof(p));
    uint8_t s[2] = { SCAN_PHY_1M, SCAN_DISCOVER_OBSERVE };
    bgapi_send(CMD_SCANNER_START, s, sizeof(s));
}

static void ep_close(void)
{
    if (B.ep_open) {
        cpc_close_endpoint(&B.ep);
        B.ep_open = false;
        B.ep_fd = -1;
    }
}

static void ep_try_open(uint64_t now)
{
    if (B.ep_open || !link_up() || now < B.ep_next_try) {
        return;
    }
    int fd = cpc_open_endpoint(link_handle(), &B.ep, IOTD_BLE_ENDPOINT, 1);
    if (fd < 0) {
        if (!B.ep_failure_logged) {
            LOGI("ble: open endpoint %d failed: %d, retrying", IOTD_BLE_ENDPOINT, fd);
            B.ep_failure_logged = true;
        }
        B.ep_next_try = now + EP_RETRY_MS;
        return;
    }
    B.ep_fd = fd;
    B.ep_open = true;
    B.ep_failure_logged = false;
    B.ep_generation = link_generation();
    LOGI("ble: endpoint %d open (fd %d)", IOTD_BLE_ENDPOINT, fd);
    scan_setup();
}

int ble_init(const struct config *c)
{
    memset(&B, 0, sizeof(B));
    B.ep_fd = -1;
    B.listen_idx = -1;
    B.ep_idx = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_init(&B.clients[i], CLIENT_BUF);
        B.client_idx[i] = -1;
    }
    B.listen_fd = net_listen(c->ble_port);
    if (B.listen_fd < 0) {
        return -1;
    }
    LOGI("ble: advertisement stream on TCP port %d", c->ble_port);
    return 0;
}

void ble_link_down(void)
{
    ep_close();
    B.ep_next_try = 0;
}

static void broadcast(const char *line, size_t n)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &B.clients[i];
        if (c->fd < 0) {
            continue;
        }
        unsigned before = c->dropped;
        if (client_queue(c, (const uint8_t *)line, n, true) < 0 || client_flush(c) < 0) {
            LOGI("ble: client fd %d closed", c->fd);
            client_close(c);
            continue;
        }
        if (c->dropped != before) {
            B.dropped += c->dropped - before;
            if ((c->dropped & (c->dropped - 1)) == 0) { /* log at 1, 2, 4, 8, ... */
                LOGW("ble: client fd %d is slow, %u records dropped so far", c->fd, c->dropped);
            }
        }
    }
}

static const char *b2s(bool b) { return b ? "true" : "false"; }

/* Emit one JSON record. addr is the 6-byte little-endian bd_addr. */
static void emit_record(uint64_t now, uint8_t flags, const uint8_t *addr, uint8_t addr_type, int8_t rssi,
                        uint8_t channel, bool extended, int tx_power, int data_status, const uint8_t *data,
                        size_t dlen)
{
    char line[1024];
    size_t off = 0;
    off += (size_t)snprintf(line + off, sizeof(line) - off,
                            "{\"ts_ms\":%llu,\"address\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"address_type\":%u,"
                            "\"rssi\":%d,\"channel\":%u,\"connectable\":%s,\"scannable\":%s,\"directed\":%s,"
                            "\"scan_response\":%s,\"extended\":%s",
                            (unsigned long long)now, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], addr_type,
                            rssi, channel, b2s(flags & 1), b2s(flags & 2), b2s(flags & 4), b2s(flags & 8),
                            b2s(extended));
    if (extended) {
        off += (size_t)snprintf(line + off, sizeof(line) - off, ",\"tx_power\":%d,\"data_status\":%d", tx_power,
                                data_status);
    }
    off += (size_t)snprintf(line + off, sizeof(line) - off, ",\"data\":\"");
    for (size_t i = 0; i < dlen && off + 4 < sizeof(line); i++) {
        off += (size_t)snprintf(line + off, sizeof(line) - off, "%02x", data[i]);
    }
    off += (size_t)snprintf(line + off, sizeof(line) - off, "\"}\n");
    if (off >= sizeof(line)) {
        return;
    }
    B.adverts++;
    broadcast(line, off);
}

static void handle_message(uint64_t now, uint32_t id, const uint8_t *p, size_t len)
{
    switch (id) {
    case EVT_SYSTEM_BOOT:
        B.boots++;
        if (len >= 8) {
            LOGI("ble: system_boot: Bluetooth stack %u.%u.%u build %u", p[0] | (p[1] << 8), p[2] | (p[3] << 8),
                 p[4] | (p[5] << 8), p[6] | (p[7] << 8));
        } else {
            LOGI("ble: system_boot");
        }
        scan_setup();
        break;
    case EVT_SCANNER_LEGACY_REPORT:
        /* flags(1) addr(6) addr_type(1) bonding(1) rssi(1) channel(1) target(6) target_type(1) data(1+n) */
        if (len >= 19 && len >= 19u + p[18]) {
            B.legacy++;
            emit_record(now, p[0], p + 1, p[7], (int8_t)p[9], p[10], false, 0, 0, p + 19, p[18]);
        } else {
            LOGW("ble: short legacy report (%zu bytes)", len);
        }
        break;
    case EVT_SCANNER_EXTENDED_REPORT:
        /* legacy fields (18) adv_sid(1) primary_phy(1) secondary_phy(1) tx_power(1) periodic_interval(2)
         * data_completeness(1) counter(1) data(1+n) */
        if (len >= 27 && len >= 27u + p[26]) {
            B.extended++;
            emit_record(now, p[0], p + 1, p[7], (int8_t)p[9], p[10], true, (int8_t)p[21], p[24], p + 27, p[26]);
        } else {
            LOGW("ble: short extended report (%zu bytes)", len);
        }
        break;
    case EVT_SYSTEM_ERROR:
        LOGW("ble: system_error reason 0x%04x", len >= 2 ? (p[0] | (p[1] << 8)) : 0);
        break;
    case CMD_SYSTEM_GET_VERSION:
        /* responses start with the 16-bit sl_status_t result, events do not */
        if (len >= 10) {
            LOGI("ble: NCP Bluetooth stack %u.%u.%u build %u", p[2] | (p[3] << 8), p[4] | (p[5] << 8),
                 p[6] | (p[7] << 8), p[8] | (p[9] << 8));
        }
        break;
    case CMD_SCANNER_SET_PARAMETERS:
    case CMD_SCANNER_START:
    case CMD_SCANNER_STOP: {
        unsigned st = len >= 2 ? (unsigned)(p[0] | (p[1] << 8)) : 0xFFFF;
        const char *what = id == CMD_SCANNER_START ? "scanner_start"
                           : id == CMD_SCANNER_STOP ? "scanner_stop" : "scanner_set_parameters";
        if (st == 0) {
            LOGI("ble: %s ok", what);
        } else if (id == CMD_SCANNER_STOP) {
            LOGD("ble: %s status 0x%04x", what, st);
        } else {
            LOGW("ble: %s status 0x%04x", what, st);
        }
        break;
    }
    default:
        B.events_other++;
        LOGD("ble: message id 0x%08x len %zu", id, len);
        break;
    }
}

static void ep_read(uint64_t now)
{
    for (int burst = 0; burst < 32; burst++) {
        ssize_t n = cpc_read_endpoint(B.ep, B.rx, sizeof(B.rx), CPC_ENDPOINT_READ_FLAG_NON_BLOCKING);
        if (n < 0) {
            if (-n == EAGAIN || -n == EWOULDBLOCK) {
                return;
            }
            log_msg(cpcd_restarting() ? LVL_INFO : LVL_ERR, "ble: cpc_read_endpoint: %zd%s", n,
                    cpcd_restarting() ? " (cpcd restart in progress)" : "");
            link_request_down(cpcd_restarting() ? "cpcd restarting" : "ble read failed");
            return;
        }
        if (n == 0) {
            return;
        }
        log_hex(LVL_DEBUG, "ble: rx", B.rx, (size_t)n);
        size_t off = 0;
        while (off + 4 <= (size_t)n) {
            uint32_t hdr = (uint32_t)B.rx[off] | ((uint32_t)B.rx[off + 1] << 8) | ((uint32_t)B.rx[off + 2] << 16) |
                           ((uint32_t)B.rx[off + 3] << 24);
            size_t len = (size_t)B.rx[off + 1] | ((size_t)(B.rx[off] & 7) << 8);
            if (off + 4 + len > (size_t)n) {
                LOGW("ble: truncated BGAPI message (%zu + %zu > %zd)", off + 4, len, n);
                break;
            }
            handle_message(now, hdr & BGAPI_ID_MASK, B.rx + off + 4, len);
            off += 4 + len;
        }
    }
}

void ble_tick(uint64_t now)
{
    if (B.ep_open && B.ep_generation != link_generation()) {
        ep_close(); /* handle belongs to a previous cpc_init */
    }
    ep_try_open(now);
    if (B.stats_at == 0) {
        B.stats_at = now;
    } else if (now - B.stats_at >= STATS_PERIOD_MS) {
        B.stats_at = now;
        LOGI("ble: %lu advertisements (%lu legacy, %lu extended), %lu dropped for slow clients, %d clients, %lu boots",
             B.adverts, B.legacy, B.extended, B.dropped, clients_connected(), B.boots);
    }
}

void ble_poll_add(struct pollset *ps)
{
    B.listen_idx = pollset_add(ps, B.listen_fd, POLLIN);
    B.ep_idx = B.ep_open ? pollset_add(ps, B.ep_fd, POLLIN) : -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &B.clients[i];
        B.client_idx[i] = c->fd >= 0 ? pollset_add(ps, c->fd, POLLIN | (c->len ? POLLOUT : 0)) : -1;
    }
}

void ble_poll_handle(const struct pollset *ps, uint64_t now)
{
    if (pollset_revents(ps, B.listen_idx) & POLLIN) {
        int fd = net_accept(B.listen_fd);
        if (fd >= 0) {
            int slot = -1;
            for (int i = 0; i < MAX_CLIENTS && slot < 0; i++) {
                if (B.clients[i].fd < 0) {
                    slot = i;
                }
            }
            if (slot < 0) {
                LOGW("ble: client limit (%d) reached, refusing connection", MAX_CLIENTS);
                close(fd);
            } else {
                B.clients[slot].fd = fd;
                B.clients[slot].len = 0;
                B.clients[slot].dropped = 0;
                B.clients[slot].connected_at = now;
                LOGI("ble: client connected (fd %d, %d clients)", fd, clients_connected());
            }
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = &B.clients[i];
        short re = pollset_revents(ps, B.client_idx[i]);
        if (c->fd < 0 || !re) {
            continue;
        }
        if (re & (POLLIN | POLLERR | POLLHUP)) {
            uint8_t sink[256];
            ssize_t r = read(c->fd, sink, sizeof(sink)); /* input is ignored; EOF closes */
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
                LOGI("ble: client fd %d disconnected (%u records dropped while connected)", c->fd, c->dropped);
                client_close(c);
                continue;
            }
        }
        if ((re & POLLOUT) && client_flush(c) < 0) {
            LOGI("ble: client fd %d closed", c->fd);
            client_close(c);
        }
    }
    if (B.ep_open && (pollset_revents(ps, B.ep_idx) & (POLLIN | POLLERR | POLLHUP))) {
        ep_read(now);
    }
}

void ble_shutdown(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_close(&B.clients[i]);
    }
    ep_close();
    if (B.listen_fd >= 0) {
        close(B.listen_fd);
        B.listen_fd = -1;
    }
}
