/* A minimal mDNS responder (RFC 6762/6763) for the AP's ESPHome Bluetooth proxy: one _esphomelib._tcp
 * service, so Home Assistant discovers the AP as it discovers ESPHome devices.
 *
 * Records: PTR _esphomelib._tcp.local -> <name>._esphomelib._tcp.local; SRV -> <name>.local:<api_port>;
 * TXT friendly_name, version, mac, platform, network, api_encryption (what ESPHome devices publish and Home
 * Assistant reads at discovery); A <name>.local -> the AP's IPv4 address. Also PTR _services._dns-sd._udp.local.
 * Announced at start (0, 1, 3 s) and every 5 minutes or when the address changes; a goodbye (TTL 0) on exit.
 * Queries for those names are answered by multicast, or by unicast to a querier not on port 5353 (legacy).
 * The socket shares port 5353 (SO_REUSEADDR) with the firmware's own mdnsd when its Bonjour gateway runs.
 * On a gateway AP the NAT firewall would drop mDNS; see firewall_check below.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "iotd.h"

#define MDNS_PORT       5353
#define TTL_HOST        120
#define TTL_OTHER       4500
#define ANNOUNCE_MS     (5 * 60 * 1000)
#define TYPE_A          1
#define TYPE_PTR        12
#define TYPE_TXT        16
#define TYPE_SRV        33
#define TYPE_ANY        255

static const char SERVICE[] = "_esphomelib._tcp.local";
static const char SERVICES[] = "_services._dns-sd._udp.local";

static struct {
    bool on;
    const struct config *cfg;
    int fd, idx;
    char instance[96], host[64];
    uint32_t addr; /* network order, 0 while unknown */
    uint64_t addr_checked_at;
    uint64_t next_announce;
    int burst;     /* start-up announcements still to send */
} M;

/* The IPv4 address the AP would send multicast from (no packet is sent by a UDP connect). */
static uint32_t local_address(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return 0;
    }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
    inet_pton(AF_INET, "224.0.0.251", &sa.sin_addr);
    uint32_t addr = 0;
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        socklen_t sl = sizeof(sa);
        if (getsockname(s, (struct sockaddr *)&sa, &sl) == 0) {
            addr = sa.sin_addr.s_addr;
        }
    }
    close(s);
    return addr;
}

/* ---- writing ---- */
struct msg {
    uint8_t b[1400];
    size_t n;
    bool overflow;
};

static void put(struct msg *m, const void *d, size_t n)
{
    if (m->n + n > sizeof(m->b)) {
        m->overflow = true;
        return;
    }
    memcpy(m->b + m->n, d, n);
    m->n += n;
}

static void put16(struct msg *m, unsigned v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    put(m, b, 2);
}

static void put32(struct msg *m, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put(m, b, 4);
}

/* A dotted name as labels (no compression). */
static void put_name(struct msg *m, const char *name)
{
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t n = dot ? (size_t)(dot - name) : strlen(name);
        uint8_t len = (uint8_t)n;
        put(m, &len, 1);
        put(m, name, n);
        name += n + (dot ? 1 : 0);
    }
    uint8_t zero = 0;
    put(m, &zero, 1);
}

/* Starts a record; returns the offset of its RDLENGTH, filled in by end_rr. */
static size_t begin_rr(struct msg *m, const char *name, unsigned type, bool unique, uint32_t ttl)
{
    put_name(m, name);
    put16(m, type);
    put16(m, unique ? 0x8001 : 0x0001); /* class IN, cache-flush for records only we own */
    put32(m, ttl);
    size_t at = m->n;
    put16(m, 0);
    return at;
}

static void end_rr(struct msg *m, size_t at)
{
    size_t len = m->n - at - 2;
    if (at + 2 <= sizeof(m->b)) {
        m->b[at] = (uint8_t)(len >> 8);
        m->b[at + 1] = (uint8_t)len;
    }
}

static void txt_entry(struct msg *m, const char *key, const char *value)
{
    char e[160];
    int n = snprintf(e, sizeof(e), "%s=%s", key, value);
    if (n > 0 && n < 256) {
        uint8_t len = (uint8_t)n;
        put(m, &len, 1);
        put(m, e, (size_t)n);
    }
}

enum { R_PTR = 1, R_SRV = 2, R_TXT = 4, R_A = 8, R_SERVICES = 16 };

static void put_records(struct msg *m, unsigned which, uint32_t ttl_other, uint32_t ttl_host, bool legacy)
{
    if (legacy) {
        ttl_other = ttl_other > 10 ? 10 : ttl_other;
        ttl_host = ttl_host > 10 ? 10 : ttl_host;
    }
    size_t at;
    if (which & R_SERVICES) {
        at = begin_rr(m, SERVICES, TYPE_PTR, false, ttl_other);
        put_name(m, SERVICE);
        end_rr(m, at);
    }
    if (which & R_PTR) {
        at = begin_rr(m, SERVICE, TYPE_PTR, false, ttl_other);
        put_name(m, M.instance);
        end_rr(m, at);
    }
    if (which & R_SRV) {
        at = begin_rr(m, M.instance, TYPE_SRV, !legacy, ttl_host);
        put16(m, 0);
        put16(m, 0);
        put16(m, (unsigned)M.cfg->api_port);
        put_name(m, M.host);
        end_rr(m, at);
    }
    if (which & R_TXT) {
        const struct config *c = M.cfg;
        char mac[13];
        size_t mn = 0;
        for (const char *s = c->api_mac; *s && mn < 12; s++) {
            if (*s != ':') {
                mac[mn++] = (char)(*s >= 'A' && *s <= 'F' ? *s + 32 : *s);
            }
        }
        mac[mn] = 0;
        at = begin_rr(m, M.instance, TYPE_TXT, !legacy, ttl_other);
        txt_entry(m, "friendly_name", c->api_friendly_name[0] ? c->api_friendly_name : c->api_name);
        txt_entry(m, "version", "2025.10.0");
        txt_entry(m, "mac", mac);
        txt_entry(m, "platform", "Ruckus");
        txt_entry(m, "network", "ethernet");
        txt_entry(m, "api_encryption", "Noise_NNpsk0_25519_ChaChaPoly_SHA256");
        end_rr(m, at);
    }
    if ((which & R_A) && M.addr) {
        at = begin_rr(m, M.host, TYPE_A, !legacy, ttl_host);
        put(m, &M.addr, 4);
        end_rr(m, at);
    }
}

static int count_records(unsigned which)
{
    int n = 0;
    for (unsigned b = 1; b <= R_SERVICES; b <<= 1) {
        n += (which & b) && (b != R_A || M.addr);
    }
    return n;
}

static void send_to(const struct msg *m, const struct sockaddr_in *to)
{
    if (!m->overflow) {
        sendto(M.fd, m->b, m->n, 0, (const struct sockaddr *)to, sizeof(*to));
    }
}

static void multicast(const struct msg *m)
{
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT) };
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    send_to(m, &to);
}

/* An unsolicited response with every record (announcement), or TTL 0 (goodbye). */
static void announce(bool goodbye)
{
    struct msg m = { .n = 0 };
    unsigned all = R_PTR | R_SRV | R_TXT | R_A;
    put16(&m, 0);
    put16(&m, 0x8400);
    put16(&m, 0);
    put16(&m, (unsigned)count_records(all));
    put16(&m, 0);
    put16(&m, 0);
    put_records(&m, all, goodbye ? 0 : TTL_OTHER, goodbye ? 0 : TTL_HOST, false);
    multicast(&m);
}

/* ---- reading ---- */
/* Reads a (possibly compressed) name at *off into out as dotted lower-case text; false when malformed. */
static bool read_name(const uint8_t *p, size_t n, size_t *off, char *out, size_t cap)
{
    size_t o = *off, w = 0;
    bool jumped = false;
    int hops = 0;
    while (o < n) {
        uint8_t len = p[o];
        if (len == 0) {
            if (!jumped) {
                *off = o + 1;
            }
            if (w && out[w - 1] == '.') {
                w--;
            }
            out[w] = 0;
            return true;
        }
        if ((len & 0xC0) == 0xC0) {
            if (o + 1 >= n || ++hops > 16) {
                return false;
            }
            if (!jumped) {
                *off = o + 2;
            }
            jumped = true;
            o = ((size_t)(len & 0x3F) << 8) | p[o + 1];
            continue;
        }
        if (o + 1 + len > n || w + len + 2 > cap) {
            return false;
        }
        for (size_t i = 0; i < len; i++) {
            char ch = (char)p[o + 1 + i];
            out[w++] = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        }
        out[w++] = '.';
        o += 1 + len;
    }
    return false;
}

static void handle_query(const uint8_t *p, size_t n, const struct sockaddr_in *from)
{
    if (n < 12 || (p[2] & 0x80)) {
        return; /* not a query */
    }
    unsigned qd = ((unsigned)p[4] << 8) | p[5];
    size_t off = 12;
    unsigned answer = 0;
    for (unsigned q = 0; q < qd; q++) {
        char name[256];
        if (!read_name(p, n, &off, name, sizeof(name)) || off + 4 > n) {
            return;
        }
        unsigned type = ((unsigned)p[off] << 8) | p[off + 1];
        off += 4;
        bool any = type == TYPE_ANY;
        if (!strcasecmp(name, SERVICE) && (any || type == TYPE_PTR)) {
            answer |= R_PTR;
        } else if (!strcasecmp(name, M.instance) && (any || type == TYPE_SRV)) {
            answer |= R_SRV;
        }
        if (!strcasecmp(name, M.instance) && (any || type == TYPE_TXT)) {
            answer |= R_TXT;
        }
        if (!strcasecmp(name, M.host) && (any || type == TYPE_A)) {
            answer |= R_A;
        }
        if (!strcasecmp(name, SERVICES) && (any || type == TYPE_PTR)) {
            answer |= R_SERVICES;
        }
    }
    if (!answer) {
        return;
    }
    bool legacy = ntohs(from->sin_port) != MDNS_PORT;
    /* Additionals as ESPHome devices send them: a PTR answer carries SRV, TXT and A. */
    unsigned extra = 0;
    if (answer & R_PTR) {
        extra = (R_SRV | R_TXT | R_A) & ~answer;
    } else if (answer & R_SRV) {
        extra = R_A & ~answer;
    }
    struct msg m = { .n = 0 };
    put16(&m, legacy ? ((unsigned)p[0] << 8) | p[1] : 0);
    put16(&m, 0x8400);
    put16(&m, legacy ? qd : 0);
    put16(&m, (unsigned)count_records(answer));
    put16(&m, 0);
    put16(&m, (unsigned)count_records(extra));
    if (legacy) {
        put(&m, p + 12, off - 12); /* echo the questions */
    }
    put_records(&m, answer, TTL_OTHER, TTL_HOST, legacy);
    put_records(&m, extra, TTL_OTHER, TTL_HOST, legacy);
    if (legacy) {
        send_to(&m, from);
    } else {
        multicast(&m);
    }
}

/* ---- the gateway AP's firewall ----
 * In gateway (NAT) mode the AP's /etc/init.d/nat drops everything on the WAN bridge that is not addressed
 * to the AP itself (mangle PREROUTING "! -d <wan address> -i br0 -j DROP"), which includes mDNS multicast,
 * and it rebuilds that chain whenever NAT is (re)enabled. So while the responder runs, a rule accepting mDNS
 * is kept at the top of the chain (checked every 30 s) and removed on exit. The R560's 7.2 uses iptables
 * 1.4.21; firmware whose nat script uses nft gets the same rule in "inet mangle prerouting". APs that are
 * not gateways have no DROP rule and are left alone.
 */
#define IPTABLES "/usr/sbin/iptables"
#define NFT "/usr/sbin/nft"
#define IPT_RULE "PREROUTING -d 224.0.0.251/32 -p udp --dport 5353 -j ACCEPT"
#define FIREWALL_CHECK_MS 30000

static uint64_t firewall_checked_at;
static bool firewall_rule_added;

/* A command's output, or false when it fails or prints nothing. */
static bool run_capture(const char *cmd, char *out, size_t cap)
{
    FILE *p = popen(cmd, "r");
    if (!p) {
        return false;
    }
    size_t n = fread(out, 1, cap - 1, p);
    out[n] = 0;
    return pclose(p) == 0 && n > 0;
}

/* The nft chain's listing (with handles), or false when there is no nft or no such chain. */
static bool list_prerouting(char *out, size_t cap)
{
    return access(NFT, X_OK) == 0 && run_capture(NFT " -a list chain inet mangle prerouting 2>/dev/null", out, cap);
}

static void firewall_check(uint64_t now)
{
    if (firewall_checked_at && now - firewall_checked_at < FIREWALL_CHECK_MS) {
        return;
    }
    firewall_checked_at = now;
    char list[8192];
    bool added = false;
    if (access(IPTABLES, X_OK) == 0) {
        if (!run_capture(IPTABLES " -t mangle -S PREROUTING 2>/dev/null", list, sizeof(list)) || !strstr(list, "-j DROP")
            || system(IPTABLES " -t mangle -C " IPT_RULE " 2>/dev/null") == 0) {
            return;
        }
        added = system(IPTABLES " -t mangle -I " IPT_RULE " 2>/dev/null") == 0;
    } else {
        if (!list_prerouting(list, sizeof(list)) || !strstr(list, "drop") || strstr(list, "224.0.0.251")) {
            return;
        }
        added = system(NFT " insert rule inet mangle prerouting ip daddr 224.0.0.251 udp dport 5353 accept 2>/dev/null") == 0;
    }
    if (added) {
        LOGI("mdns: the gateway firewall dropped mDNS; added an accept rule for 224.0.0.251:5353");
        firewall_rule_added = true;
    }
}

static void firewall_remove(void)
{
    if (!firewall_rule_added) {
        return;
    }
    if (access(IPTABLES, X_OK) == 0) {
        while (system(IPTABLES " -t mangle -D " IPT_RULE " 2>/dev/null") == 0) {
        }
        return;
    }
    char list[8192];
    if (!list_prerouting(list, sizeof(list))) {
        return;
    }
    for (char *line = strtok(list, "\n"); line; line = strtok(NULL, "\n")) {
        char *h = strstr(line, "# handle ");
        if (strstr(line, "224.0.0.251") && strstr(line, "5353") && h) {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), NFT " delete rule inet mangle prerouting handle %d 2>/dev/null", atoi(h + 9));
            if (system(cmd) != 0) {
                LOGW("mdns: removing the firewall rule failed");
            }
        }
    }
}

/* ---- module interface ---- */
int mdns_init(const struct config *c)
{
    memset(&M, 0, sizeof(M));
    M.fd = M.idx = -1;
    if (c->api_port <= 0 || !c->api_mdns) {
        return 0;
    }
    M.cfg = c;
    snprintf(M.instance, sizeof(M.instance), "%s.%s", c->api_name, SERVICE);
    snprintf(M.host, sizeof(M.host), "%s.local", c->api_name);
    M.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (M.fd < 0) {
        LOGE("mdns: socket: %s", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(M.fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(M.fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(MDNS_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(M.fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        LOGE("mdns: bind 5353: %s", strerror(errno));
        close(M.fd);
        M.fd = -1;
        return -1;
    }
    unsigned char ttl = 255, loop = 0;
    setsockopt(M.fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(M.fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    M.on = true;
    M.burst = 3;
    LOGI("mdns: advertising %s", M.instance);
    return 0;
}

void mdns_tick(uint64_t now)
{
    if (!M.on) {
        return;
    }
    firewall_check(now);
    if (now < M.addr_checked_at + 10000 && M.addr_checked_at) {
        goto announce_due;
    }
    M.addr_checked_at = now;
    uint32_t addr = local_address();
    if (addr != M.addr) {
        struct ip_mreq mr;
        inet_pton(AF_INET, "224.0.0.251", &mr.imr_multiaddr);
        if (M.addr) {
            mr.imr_interface.s_addr = M.addr;
            setsockopt(M.fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mr, sizeof(mr));
        }
        M.addr = addr;
        if (addr) {
            mr.imr_interface.s_addr = addr;
            if (setsockopt(M.fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0 && errno != EADDRINUSE) {
                LOGW("mdns: joining 224.0.0.251: %s", strerror(errno));
            }
            setsockopt(M.fd, IPPROTO_IP, IP_MULTICAST_IF, &mr.imr_interface, sizeof(mr.imr_interface));
            const uint8_t *p = (const uint8_t *)&addr;
            LOGI("mdns: %s is %u.%u.%u.%u", M.host, p[0], p[1], p[2], p[3]);
            M.burst = 3;
            M.next_announce = now;
        }
    }
announce_due:
    if (M.addr && now >= M.next_announce) {
        announce(false);
        M.burst = M.burst > 0 ? M.burst - 1 : 0;
        M.next_announce = now + (M.burst == 2 ? 1000 : M.burst == 1 ? 2000 : ANNOUNCE_MS);
    }
}

void mdns_poll_add(struct pollset *ps)
{
    M.idx = M.on ? pollset_add(ps, M.fd, POLLIN) : -1;
}

void mdns_poll_handle(const struct pollset *ps)
{
    if (!M.on || !(pollset_revents(ps, M.idx) & POLLIN)) {
        return;
    }
    uint8_t p[1500];
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    ssize_t r = recvfrom(M.fd, p, sizeof(p), MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (r > 0) {
        handle_query(p, (size_t)r, &from);
    }
}

void mdns_shutdown(void)
{
    if (!M.on) {
        return;
    }
    if (M.addr) {
        announce(true);
    }
    firewall_remove();
    close(M.fd);
    M.fd = -1;
    M.on = false;
}
