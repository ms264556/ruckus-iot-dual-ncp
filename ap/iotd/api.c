/* The AP's own ESPHome Bluetooth proxy: the ESPHome native API with Noise encryption, so Home Assistant adds
 * the AP as it would an ESPHome device with "api: encryption: key:".
 *
 * Transport (ESPHome's api_frame_helper_noise.cpp, aioesphomeapi's _frame_helper/noise.py): every frame is
 * 0x01, a 2-byte big-endian length and the payload.
 *   1. Client hello (any payload). Server hello: 0x01, node name, 0x00, MAC (lower-case hex), 0x00.
 *   2. Noise_NNpsk0_25519_ChaChaPoly_SHA256, prologue "NoiseAPIInit\0\0": the client's 0x00 + "-> psk, e",
 *      the server's 0x00 + "<- e, ee". A wrong key fails the first message's tag; the server answers
 *      0x01 + "Handshake MAC failure", which the client reports as an invalid encryption key.
 *   3. Data: ChaChaPoly ciphertext (no associated data) of a 2-byte big-endian message type, a 2-byte
 *      big-endian length and the protobuf message; nonce = 4 zero bytes + a 64-bit little-endian counter.
 *
 * Messages (ESPHome api.proto): hello, connect, device info, list entities (none), ping, get time, disconnect,
 * and the Bluetooth LE advertisement subscription with raw advertisements in batches of up to 16, sent every
 * 100 ms. A passive proxy: no GATT connections (connections free 0 of 0). The same message set as the
 * controller's own proxy (RuckusMiniController Services/Iot/EsphomeProxyServer.cs).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "crypto.h"
#include "iotd.h"

#define MAX_API_CLIENTS   4
#define TX_BUF            (96 * 1024)
#define RX_MAX            (3 + 0xFFFF)
#define BATCH_MAX         16
#define BATCH_MS          100
#define HANDSHAKE_MS      30000
#define ADV_DATA_MAX      62

/* api.proto message ids */
enum {
    HELLO_REQ = 1, HELLO_RESP = 2, CONNECT_REQ = 3, CONNECT_RESP = 4, DISCONNECT_REQ = 5, DISCONNECT_RESP = 6,
    PING_REQ = 7, PING_RESP = 8, DEVICE_INFO_REQ = 9, DEVICE_INFO_RESP = 10, LIST_ENTITIES_REQ = 11,
    LIST_ENTITIES_DONE = 19, GET_TIME_REQ = 36, GET_TIME_RESP = 37, BLE_SUBSCRIBE_REQ = 66,
    BLE_CONNECTIONS_FREE_REQ = 80, BLE_CONNECTIONS_FREE_RESP = 81, BLE_UNSUBSCRIBE_REQ = 87, BLE_RAW_ADVERTS = 93,
};

#define API_VERSION_MAJOR 1
#define API_VERSION_MINOR 10
#define REPORTED_ESPHOME_VERSION "2025.10.0"
#define FEATURE_FLAGS     ((1u << 0) | (1u << 5)) /* passive scan, raw advertisements */

static const char PROTOCOL_NAME[] = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";
static const uint8_t PROLOGUE[] = "NoiseAPIInit\0"; /* 14 bytes with the implicit terminator */

struct cipher {
    uint8_t k[32];
    uint64_t n;
};

struct handshake {
    uint8_t ck[32], h[32];
    bool has_k;
    struct cipher c;
    uint8_t re[32];
};

struct api_client {
    struct client out;
    uint8_t *rx;
    size_t rx_len;
    enum { PH_HELLO, PH_HANDSHAKE, PH_DATA } phase;
    struct handshake hs;
    struct cipher send, recv;
    bool subscribed;
    char peer[24];
    uint64_t since;
    unsigned dropped;
};

struct advert {
    uint64_t address;
    int32_t rssi;
    uint32_t type;
    uint8_t len;
    uint8_t data[ADV_DATA_MAX];
};

static struct {
    bool on;
    const struct config *cfg;
    int listen_fd, listen_idx;
    struct api_client cl[MAX_API_CLIENTS];
    int cl_idx[MAX_API_CLIENTS];
    struct advert batch[BATCH_MAX];
    int batch_n;
    uint64_t batch_at;
    unsigned long adverts_sent, adverts_dropped, wrong_key;
} A;

/* ---- Noise ---- */
static void nonce_of(uint64_t n, uint8_t nonce[12])
{
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = (uint8_t)(n >> (8 * i));
    }
}

static void mix_hash(struct handshake *s, const uint8_t *d, size_t n)
{
    struct sha256 h;
    sha256_init(&h);
    sha256_update(&h, s->h, 32);
    sha256_update(&h, d, n);
    sha256_final(&h, s->h);
}

/* Noise §4.3 HKDF with two or three outputs. */
static void hkdf(const uint8_t ck[32], const uint8_t *ikm, size_t n, uint8_t o1[32], uint8_t o2[32], uint8_t *o3)
{
    uint8_t temp[32], b[33];
    hmac_sha256(ck, 32, ikm, n, NULL, 0, temp);
    b[0] = 1;
    hmac_sha256(temp, 32, b, 1, NULL, 0, o1);
    memcpy(b, o1, 32);
    b[32] = 2;
    hmac_sha256(temp, 32, b, 33, NULL, 0, o2);
    if (o3) {
        memcpy(b, o2, 32);
        b[32] = 3;
        hmac_sha256(temp, 32, b, 33, NULL, 0, o3);
    }
}

static void mix_key(struct handshake *s, const uint8_t *ikm, size_t n)
{
    uint8_t ck[32];
    hkdf(s->ck, ikm, n, ck, s->c.k, NULL);
    memcpy(s->ck, ck, 32);
    s->c.n = 0;
    s->has_k = true;
}

static void mix_key_and_hash(struct handshake *s, const uint8_t *ikm, size_t n)
{
    uint8_t ck[32], th[32];
    hkdf(s->ck, ikm, n, ck, th, s->c.k);
    memcpy(s->ck, ck, 32);
    mix_hash(s, th, 32);
    s->c.n = 0;
    s->has_k = true;
}

static void handshake_init(struct handshake *s)
{
    memset(s, 0, sizeof(*s));
    sha256(PROTOCOL_NAME, strlen(PROTOCOL_NAME), s->h); /* 36 bytes: longer than HASHLEN, so hashed */
    memcpy(s->ck, s->h, 32);
    mix_hash(s, PROLOGUE, sizeof(PROLOGUE));
}

/* -> psk, e (and an encrypted payload, usually empty). False when the tag does not verify. */
static bool read_message1(struct handshake *s, const uint8_t *key, const uint8_t *m, size_t n)
{
    if (n < 32 + AEAD_TAG_LEN || n > 32 + AEAD_TAG_LEN + 256) {
        return false;
    }
    mix_key_and_hash(s, key, 32);
    memcpy(s->re, m, 32);
    mix_hash(s, s->re, 32);
    mix_key(s, s->re, 32);
    uint8_t nonce[12], payload[256];
    nonce_of(s->c.n, nonce);
    size_t pn = n - 32 - AEAD_TAG_LEN;
    if (!aead_decrypt(s->c.k, nonce, s->h, 32, m + 32, pn, payload)) {
        return false;
    }
    s->c.n++;
    mix_hash(s, m + 32, n - 32);
    return true;
}

/* <- e, ee into out (48 bytes), then Split: the responder receives with the first key and sends with the second. */
static bool write_message2(struct handshake *s, uint8_t out[48], struct cipher *recv, struct cipher *send)
{
    uint8_t e_priv[32], e_pub[32], dh[32];
    if (!random_bytes(e_priv, 32)) {
        return false;
    }
    x25519(e_pub, e_priv, NULL);
    mix_hash(s, e_pub, 32);
    mix_key(s, e_pub, 32);
    x25519(dh, e_priv, s->re);
    static const uint8_t zero[32];
    if (equal_ct(dh, zero, 32)) {
        return false; /* RFC 7748 §6.1: a low-order point */
    }
    mix_key(s, dh, 32);
    memcpy(out, e_pub, 32);
    uint8_t nonce[12];
    nonce_of(s->c.n++, nonce);
    aead_encrypt(s->c.k, nonce, s->h, 32, NULL, 0, out + 32);
    mix_hash(s, out + 32, AEAD_TAG_LEN);
    hkdf(s->ck, NULL, 0, recv->k, send->k, NULL);
    recv->n = send->n = 0;
    memset(e_priv, 0, sizeof(e_priv));
    memset(dh, 0, sizeof(dh));
    return true;
}

/* ---- protobuf ---- */
struct pb {
    uint8_t *b;
    size_t n, cap;
    bool overflow;
};

static void pb_raw(struct pb *p, const void *d, size_t n)
{
    if (p->n + n > p->cap) {
        p->overflow = true;
        return;
    }
    memcpy(p->b + p->n, d, n);
    p->n += n;
}

static void pb_varint(struct pb *p, uint64_t v)
{
    uint8_t b[10];
    size_t n = 0;
    while (v >= 0x80) {
        b[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    b[n++] = (uint8_t)v;
    pb_raw(p, b, n);
}

static void pb_uint(struct pb *p, unsigned field, uint64_t v)
{
    if (v) {
        pb_varint(p, (uint64_t)field << 3);
        pb_varint(p, v);
    }
}

static void pb_bytes(struct pb *p, unsigned field, const void *d, size_t n)
{
    pb_varint(p, ((uint64_t)field << 3) | 2);
    pb_varint(p, n);
    pb_raw(p, d, n);
}

static void pb_str(struct pb *p, unsigned field, const char *s)
{
    if (s && *s) {
        pb_bytes(p, field, s, strlen(s));
    }
}

/* The varint field of a request, or 0. */
static uint64_t pb_read_uint(const uint8_t *m, size_t n, unsigned want)
{
    size_t i = 0;
    while (i < n) {
        uint64_t tag = 0;
        for (int s = 0; i < n && s < 64; s += 7) {
            uint8_t b = m[i++];
            tag |= (uint64_t)(b & 0x7f) << s;
            if (!(b & 0x80)) {
                break;
            }
        }
        unsigned wire = tag & 7, field = (unsigned)(tag >> 3);
        uint64_t v = 0;
        if (wire == 0) {
            for (int s = 0; i < n && s < 64; s += 7) {
                uint8_t b = m[i++];
                v |= (uint64_t)(b & 0x7f) << s;
                if (!(b & 0x80)) {
                    break;
                }
            }
            if (field == want) {
                return v;
            }
        } else if (wire == 2) {
            for (int s = 0; i < n && s < 64; s += 7) {
                uint8_t b = m[i++];
                v |= (uint64_t)(b & 0x7f) << s;
                if (!(b & 0x80)) {
                    break;
                }
            }
            i += v;
        } else if (wire == 5) {
            i += 4;
        } else if (wire == 1) {
            i += 8;
        } else {
            return 0;
        }
    }
    return 0;
}

/* ---- framing ---- */
static void client_drop_api(struct api_client *c, const char *why)
{
    if (c->out.fd < 0) {
        return;
    }
    LOGI("api: %s closed: %s%s", c->peer, why, c->dropped ? " (some adverts were dropped for a slow client)" : "");
    client_close(&c->out);
    free(c->rx);
    c->rx = NULL;
    c->rx_len = 0;
    c->subscribed = false;
    memset(&c->hs, 0, sizeof(c->hs));
    memset(&c->send, 0, sizeof(c->send));
    memset(&c->recv, 0, sizeof(c->recv));
}

static int queue_frame(struct api_client *c, const uint8_t *payload, size_t n)
{
    uint8_t hdr[3] = { 0x01, (uint8_t)(n >> 8), (uint8_t)n };
    if (client_queue(&c->out, hdr, 3, false) < 0 || client_queue(&c->out, payload, n, false) < 0) {
        return -1;
    }
    return client_flush(&c->out);
}

static void reject(struct api_client *c, const char *reason)
{
    uint8_t m[64];
    size_t n = strlen(reason);
    m[0] = 0x01;
    memcpy(m + 1, reason, n);
    queue_frame(c, m, n + 1);
}

/* Encrypts one message; with best_effort set, a full output buffer drops it instead of closing the client. */
static int send_msg(struct api_client *c, unsigned type, const uint8_t *msg, size_t n, bool best_effort)
{
    static uint8_t plain[4 + 0xFFFF], sealed[4 + 0xFFFF + AEAD_TAG_LEN];
    if (n + 4 + AEAD_TAG_LEN > 0xFFFF) {
        return -1;
    }
    if (best_effort && c->out.len + 3 + n + 4 + AEAD_TAG_LEN > c->out.cap) {
        c->dropped++;
        return 0; /* the cipher counter is untouched, so the stream stays in step */
    }
    plain[0] = (uint8_t)(type >> 8);
    plain[1] = (uint8_t)type;
    plain[2] = (uint8_t)(n >> 8);
    plain[3] = (uint8_t)n;
    memcpy(plain + 4, msg, n);
    uint8_t nonce[12];
    nonce_of(c->send.n++, nonce);
    aead_encrypt(c->send.k, nonce, NULL, 0, plain, n + 4, sealed);
    return queue_frame(c, sealed, n + 4 + AEAD_TAG_LEN);
}

static int send_pb(struct api_client *c, unsigned type, const struct pb *p)
{
    return p->overflow ? -1 : send_msg(c, type, p->b, p->n, false);
}

/* ---- requests ---- */
static int device_info(struct api_client *c)
{
    const struct config *k = A.cfg;
    uint8_t buf[512];
    struct pb p = { buf, 0, sizeof(buf), false };
    pb_str(&p, 2, k->api_name);
    pb_str(&p, 3, k->api_mac);
    pb_str(&p, 4, REPORTED_ESPHOME_VERSION);
    pb_str(&p, 6, k->api_model);
    pb_str(&p, 12, "Ruckus");
    pb_str(&p, 13, k->api_friendly_name);
    pb_uint(&p, 15, FEATURE_FLAGS);
    pb_str(&p, 18, k->api_bluetooth_mac);
    pb_uint(&p, 19, 1); /* api_encryption_supported */
    return send_pb(c, DEVICE_INFO_RESP, &p);
}

/* False ends the connection. */
static bool handle_message(struct api_client *c, unsigned type, const uint8_t *m, size_t n)
{
    uint8_t buf[256];
    struct pb p = { buf, 0, sizeof(buf), false };
    switch (type) {
    case HELLO_REQ:
        pb_uint(&p, 1, API_VERSION_MAJOR);
        pb_uint(&p, 2, API_VERSION_MINOR);
        pb_str(&p, 3, "iotd (ruckus-iot-dual-ncp)");
        pb_str(&p, 4, A.cfg->api_name);
        LOGI("api: %s hello (API %llu.%llu)", c->peer, (unsigned long long)pb_read_uint(m, n, 2),
             (unsigned long long)pb_read_uint(m, n, 3));
        return send_pb(c, HELLO_RESP, &p) == 0;
    case CONNECT_REQ:
        return send_pb(c, CONNECT_RESP, &p) == 0; /* no password: invalid_password stays false */
    case DEVICE_INFO_REQ:
        return device_info(c) == 0;
    case LIST_ENTITIES_REQ:
        return send_pb(c, LIST_ENTITIES_DONE, &p) == 0;
    case PING_REQ:
        return send_pb(c, PING_RESP, &p) == 0;
    case GET_TIME_REQ: {
        uint32_t t = (uint32_t)time(NULL);
        uint8_t f[5] = { (1 << 3) | 5, (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24) };
        return send_msg(c, GET_TIME_RESP, f, sizeof(f), false) == 0;
    }
    case BLE_SUBSCRIBE_REQ:
        c->subscribed = true;
        LOGI("api: %s subscribed to Bluetooth advertisements (flags %llu)", c->peer,
             (unsigned long long)pb_read_uint(m, n, 1));
        return true;
    case BLE_UNSUBSCRIBE_REQ:
        c->subscribed = false;
        return true;
    case BLE_CONNECTIONS_FREE_REQ:
        return send_pb(c, BLE_CONNECTIONS_FREE_RESP, &p) == 0; /* free 0, limit 0 */
    case DISCONNECT_REQ:
        send_pb(c, DISCONNECT_RESP, &p);
        return false;
    case DISCONNECT_RESP:
        return false;
    default:
        return true; /* states, services, logs: nothing to send for a device without entities */
    }
}

/* One frame's payload in the client's current phase. False ends the connection (why says how). */
static bool handle_frame(struct api_client *c, const uint8_t *f, size_t n, const char **why)
{
    switch (c->phase) {
    case PH_HELLO: {
        uint8_t m[128];
        size_t nn = strlen(A.cfg->api_name), mn = 0;
        char mac[13] = "";
        for (const char *s = A.cfg->api_mac; *s && mn < 12; s++) {
            if (*s != ':') {
                mac[mn++] = (char)(*s >= 'A' && *s <= 'F' ? *s + 32 : *s);
            }
        }
        mac[mn] = 0;
        m[0] = 0x01;
        memcpy(m + 1, A.cfg->api_name, nn);
        m[1 + nn] = 0;
        memcpy(m + 2 + nn, mac, mn);
        m[2 + nn + mn] = 0;
        if (queue_frame(c, m, 3 + nn + mn) < 0) {
            *why = "write failed";
            return false;
        }
        handshake_init(&c->hs);
        c->phase = PH_HANDSHAKE;
        return true;
    }
    case PH_HANDSHAKE: {
        if (n == 0) {
            reject(c, "Empty handshake message");
            *why = "empty handshake message";
            return false;
        }
        if (f[0] != 0x00) {
            reject(c, "Bad handshake error byte");
            *why = "bad handshake error byte";
            return false;
        }
        if (!read_message1(&c->hs, A.cfg->api_key, f + 1, n - 1)) {
            A.wrong_key++;
            LOGW("api: %s used a different encryption key (Handshake MAC failure)", c->peer);
            reject(c, "Handshake MAC failure");
            *why = "wrong encryption key";
            return false;
        }
        uint8_t m[49];
        m[0] = 0x00;
        if (!write_message2(&c->hs, m + 1, &c->recv, &c->send) || queue_frame(c, m, sizeof(m)) < 0) {
            *why = "handshake failed";
            return false;
        }
        memset(&c->hs, 0, sizeof(c->hs));
        c->phase = PH_DATA;
        LOGI("api: %s encrypted session established", c->peer);
        return true;
    }
    default: {
        static uint8_t plain[0xFFFF];
        if (n < 4 + AEAD_TAG_LEN) {
            *why = "short frame";
            return false;
        }
        uint8_t nonce[12];
        nonce_of(c->recv.n, nonce);
        if (!aead_decrypt(c->recv.k, nonce, NULL, 0, f, n - AEAD_TAG_LEN, plain)) {
            *why = "a frame did not decrypt";
            return false;
        }
        c->recv.n++;
        size_t len = ((size_t)plain[2] << 8) | plain[3];
        if (4 + len > n - AEAD_TAG_LEN) {
            *why = "malformed message";
            return false;
        }
        if (!handle_message(c, ((unsigned)plain[0] << 8) | plain[1], plain + 4, len)) {
            *why = "disconnect requested";
            return false;
        }
        return true;
    }
    }
}

static void client_read_api(struct api_client *c, uint64_t now)
{
    ssize_t r = read(c->out.fd, c->rx + c->rx_len, RX_MAX - c->rx_len);
    if (r == 0) {
        client_drop_api(c, "closed by the client");
        return;
    }
    if (r < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            client_drop_api(c, strerror(errno));
        }
        return;
    }
    c->rx_len += (size_t)r;
    size_t off = 0;
    while (c->out.fd >= 0 && c->rx_len - off >= 1) {
        if (c->rx[off] != 0x01) {
            /* A plaintext client (indicator 0x00): ESPHome's reject, which aioesphomeapi reports as "requires encryption". */
            reject(c, "Bad indicator byte");
            client_drop_api(c, c->rx[off] == 0x00 ? "plaintext client refused (the API is encrypted)" : "invalid frame");
            return;
        }
        if (c->rx_len - off < 3) {
            break;
        }
        size_t len = ((size_t)c->rx[off + 1] << 8) | c->rx[off + 2];
        if (c->rx_len - off < 3 + len) {
            break;
        }
        const char *why = NULL;
        if (!handle_frame(c, c->rx + off + 3, len, &why)) {
            client_drop_api(c, why);
            return;
        }
        off += 3 + len;
    }
    if (c->out.fd >= 0 && off) {
        memmove(c->rx, c->rx + off, c->rx_len - off);
        c->rx_len -= off;
    }
    (void)now;
}

/* ---- adverts ---- */
static void flush_batch(void)
{
    if (A.batch_n == 0) {
        return;
    }
    static uint8_t msg[BATCH_MAX * (ADV_DATA_MAX + 32)];
    struct pb p = { msg, 0, sizeof(msg), false };
    for (int i = 0; i < A.batch_n; i++) {
        const struct advert *a = &A.batch[i];
        uint8_t sub[ADV_DATA_MAX + 32];
        struct pb s = { sub, 0, sizeof(sub), false };
        pb_uint(&s, 1, a->address);
        pb_uint(&s, 2, ((uint32_t)a->rssi << 1) ^ (uint32_t)(a->rssi >> 31)); /* sint32 zig-zag */
        pb_uint(&s, 3, a->type);
        pb_bytes(&s, 4, a->data, a->len);
        pb_bytes(&p, 1, sub, s.n);
    }
    for (int i = 0; i < MAX_API_CLIENTS; i++) {
        struct api_client *c = &A.cl[i];
        if (c->out.fd < 0 || c->phase != PH_DATA || !c->subscribed) {
            continue;
        }
        if (send_msg(c, BLE_RAW_ADVERTS, p.b, p.n, true) < 0) {
            client_drop_api(c, "write failed");
        }
    }
    A.adverts_sent += (unsigned long)A.batch_n;
    A.batch_n = 0;
}

void api_advert(uint64_t now, const uint8_t addr[6], uint8_t addr_type, int8_t rssi, const uint8_t *data, size_t len)
{
    if (!A.on || addr_type == 255) {
        return; /* anonymous extended advertisement: no address to report */
    }
    bool any = false;
    for (int i = 0; i < MAX_API_CLIENTS && !any; i++) {
        any = A.cl[i].out.fd >= 0 && A.cl[i].subscribed;
    }
    if (!any) {
        return;
    }
    struct advert *a = &A.batch[A.batch_n];
    a->address = 0;
    for (int i = 0; i < 6; i++) {
        a->address |= (uint64_t)addr[i] << (8 * i); /* BGAPI bd_addr is little-endian */
    }
    a->rssi = rssi;
    a->type = addr_type == 0 ? 0 : 1;
    a->len = (uint8_t)(len > ADV_DATA_MAX ? ADV_DATA_MAX : len);
    memcpy(a->data, data, a->len);
    if (A.batch_n++ == 0) {
        A.batch_at = now;
    }
    if (A.batch_n == BATCH_MAX) {
        flush_batch();
    }
}

/* ---- module interface ---- */
bool api_enabled(void) { return A.on; }

int api_init(const struct config *c)
{
    memset(&A, 0, sizeof(A));
    A.listen_fd = A.listen_idx = -1;
    for (int i = 0; i < MAX_API_CLIENTS; i++) {
        client_init(&A.cl[i].out, TX_BUF);
        A.cl_idx[i] = -1;
    }
    if (c->api_port <= 0) {
        return 0;
    }
    if (!c->api_key_set || !c->api_name[0] || !c->api_mac[0]) {
        LOGE("api: api_port is set but api_key, api_name or api_mac is missing");
        return -1;
    }
    A.cfg = c;
    A.listen_fd = net_listen(c->api_port);
    if (A.listen_fd < 0) {
        return -1;
    }
    A.on = true;
    LOGI("api: ESPHome Bluetooth proxy %s on TCP port %d (Noise encrypted%s)", c->api_name, c->api_port,
         c->api_allow.n ? ", allowed addresses only" : "");
    return 0;
}

void api_tick(uint64_t now)
{
    if (!A.on) {
        return;
    }
    if (A.batch_n && now - A.batch_at >= BATCH_MS) {
        flush_batch();
    }
    for (int i = 0; i < MAX_API_CLIENTS; i++) {
        struct api_client *c = &A.cl[i];
        if (c->out.fd >= 0 && c->phase != PH_DATA && now - c->since > HANDSHAKE_MS) {
            client_drop_api(c, "no handshake within 30 s");
        }
    }
}

void api_poll_add(struct pollset *ps)
{
    if (!A.on) {
        return;
    }
    A.listen_idx = pollset_add(ps, A.listen_fd, POLLIN);
    for (int i = 0; i < MAX_API_CLIENTS; i++) {
        struct api_client *c = &A.cl[i];
        A.cl_idx[i] = c->out.fd >= 0 ? pollset_add(ps, c->out.fd, POLLIN | (c->out.len ? POLLOUT : 0)) : -1;
    }
}

void api_poll_handle(const struct pollset *ps, uint64_t now)
{
    if (!A.on) {
        return;
    }
    if (pollset_revents(ps, A.listen_idx) & POLLIN) {
        uint32_t peer;
        int fd = net_accept(A.listen_fd, &peer);
        if (fd >= 0) {
            const uint8_t *p = (const uint8_t *)&peer;
            char ip[16];
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
            int slot = -1;
            for (int i = 0; i < MAX_API_CLIENTS && slot < 0; i++) {
                if (A.cl[i].out.fd < 0) {
                    slot = i;
                }
            }
            if (!allow_match(&A.cfg->api_allow, peer)) {
                LOGW("api: refusing %s: not in api_allow", ip);
                close(fd);
            } else if (slot < 0 || !(A.cl[slot].rx = malloc(RX_MAX))) {
                LOGW("api: refusing %s: %d clients connected", ip, MAX_API_CLIENTS);
                close(fd);
            } else {
                struct api_client *c = &A.cl[slot];
                c->out.fd = fd;
                c->out.len = 0;
                c->rx_len = 0;
                c->phase = PH_HELLO;
                c->subscribed = false;
                c->dropped = 0;
                c->since = now;
                snprintf(c->peer, sizeof(c->peer), "%s", ip);
                LOGI("api: client %s connected", ip);
            }
        }
    }
    for (int i = 0; i < MAX_API_CLIENTS; i++) {
        struct api_client *c = &A.cl[i];
        short re = pollset_revents(ps, A.cl_idx[i]);
        if (c->out.fd < 0 || !re) {
            continue;
        }
        if (re & (POLLIN | POLLERR | POLLHUP)) {
            client_read_api(c, now);
        }
        if (c->out.fd >= 0 && (re & POLLOUT) && client_flush(&c->out) < 0) {
            client_drop_api(c, "write failed");
        }
    }
}

void api_shutdown(void)
{
    if (!A.on) {
        return;
    }
    for (int i = 0; i < MAX_API_CLIENTS; i++) {
        struct api_client *c = &A.cl[i];
        if (c->out.fd >= 0 && c->phase == PH_DATA) {
            uint8_t none[1];
            send_msg(c, DISCONNECT_REQ, none, 0, false); /* ESPHome says goodbye before closing */
        }
        client_drop_api(c, "shutdown");
    }
    close(A.listen_fd);
    A.listen_fd = -1;
    A.on = false;
}

void api_log_stats(void)
{
    if (A.on) {
        int n = 0, subs = 0;
        for (int i = 0; i < MAX_API_CLIENTS; i++) {
            n += A.cl[i].out.fd >= 0 && A.cl[i].phase == PH_DATA;
            subs += A.cl[i].out.fd >= 0 && A.cl[i].subscribed;
        }
        LOGI("api: %d clients (%d subscribed), %lu adverts sent, %lu wrong-key attempts", n, subs, A.adverts_sent,
             A.wrong_key);
    }
}
