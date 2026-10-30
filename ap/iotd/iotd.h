/* iotd: AP-side daemon for the dual Zigbee + Bluetooth NCP behind cpcd.
 *
 * One process, one poll() loop, no threads of our own (libcpc runs one
 * internal thread for its reset notification; we only set a flag from it).
 */
#ifndef IOTD_H
#define IOTD_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "sl_cpc.h"

/* Exported by libcpc but missing from sl_cpc.h in cpc-daemon 4.9.1. */
int cpc_deinit(cpc_handle_t *handle);

/* Fixed facts about the AP and the NCP image. Not configurable on purpose. */
#define IOTD_CPCD_INSTANCE   "cpcd_0"
#define IOTD_CPCD_LOG        "/tmp/cpcd.log"
#define IOTD_DEFAULT_CONF    "/writable/data/iotd.conf"
#define IOTD_BLE_ENDPOINT    2   /* SL_CPC_ENDPOINT_BLUETOOTH */
#define IOTD_ZB_ENDPOINT     5   /* SL_CPC_ENDPOINT_ZIGBEE */

/* ---- log.c ---- */
enum log_level { LVL_ERR = 0, LVL_WARN, LVL_INFO, LVL_DEBUG };
void log_set_verbose(bool verbose);
bool log_verbose(void);
void log_msg(enum log_level lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_hex(enum log_level lvl, const char *tag, const uint8_t *b, size_t n);
#define LOGE(...) log_msg(LVL_ERR, __VA_ARGS__)
#define LOGW(...) log_msg(LVL_WARN, __VA_ARGS__)
#define LOGI(...) log_msg(LVL_INFO, __VA_ARGS__)
#define LOGD(...) log_msg(LVL_DEBUG, __VA_ARGS__)

uint64_t now_ms(void); /* CLOCK_MONOTONIC in milliseconds */

/* IPv4 addresses or networks a listener accepts clients from. */
#define ALLOW_MAX 16
struct allow {
    int n;                  /* 0: anyone */
    uint32_t net[ALLOW_MAX], mask[ALLOW_MAX]; /* network order */
};
bool allow_parse(struct allow *a, const char *list); /* "a.b.c.d[/len]" separated by spaces or commas */
bool allow_match(const struct allow *a, uint32_t addr);

/* ---- config.c ---- */
struct config {
    bool ble_enabled;
    bool zigbee_enabled;
    int ble_port;           /* 0: no listener */
    char ble_connect[64];   /* "a.b.c.d:port" iotd connects out to and streams to; empty: none */
    int zigbee_port;        /* 0: no listener */
    char zigbee_connect[64];/* "a.b.c.d:port" iotd connects out to and serves one host on; empty: none */
    struct allow zigbee_allow; /* listener clients allowed; empty: any */
    /* The AP's own ESPHome Bluetooth proxy (api.c, mdns.c); off while api_port is 0. */
    int api_port;
    uint8_t api_key[32];    /* the Noise pre-shared key, base64 in the file */
    bool api_key_set;
    char api_name[32];      /* node name: mDNS instance and host label */
    char api_friendly_name[64];
    char api_mac[18];       /* AA:BB:CC:DD:EE:FF, Home Assistant's unique id */
    char api_model[48];
    char api_bluetooth_mac[18];
    struct allow api_allow;
    bool api_mdns;
    char cpcd_path[256];
    char cpcd_conf[256];
};
void config_defaults(struct config *c);
int config_load(struct config *c, const char *path); /* 0 ok, -1 unreadable */

/* ---- net.c ---- */
int net_listen(int port);           /* non-blocking listening socket, or -1 */
int net_accept(int listen_fd, uint32_t *peer); /* non-blocking accepted socket, or -1; peer: IPv4, network order */
/* Start a non-blocking connect to an IPv4 "a.b.c.d:port"; the socket, or -1. Finish with net_connect_result. */
int net_connect(const char *ip_port);
int net_connect_result(int fd);     /* 0 connected, else the errno the connect failed with */
bool net_parse_ip_port(const char *ip_port, uint32_t *addr, int *port); /* addr in network order */

/* A TCP client with a bounded output buffer. */
struct client {
    int fd;                 /* -1 when unused */
    uint8_t *buf;
    size_t cap;
    size_t len;
    unsigned dropped;       /* records discarded because the buffer was full */
    uint64_t connected_at;
};
void client_init(struct client *c, size_t cap);
void client_close(struct client *c);
/* Queue bytes. If drop_lines is set and the buffer is full, whole '\n'-terminated
 * records are discarded from the front until the new data fits. Without it, a
 * full buffer is a fatal condition and the client is closed. Returns 0 or -1. */
int client_queue(struct client *c, const uint8_t *data, size_t n, bool drop_lines);
int client_flush(struct client *c); /* write what fits; 0 ok, -1 closed */

/* An outbound stream (ble_connect, zigbee_connect): a non-blocking connect to "a.b.c.d:port" that writes a
 * hello line ("iotd <stream> 1\n") first, so the receiving end knows what follows. Retried 1 s doubling
 * to 30 s; a connection that lasted 30 s, or one ended with uplink_close(..., true), starts over at 1 s. */
struct uplink {
    struct client c;        /* fd >= 0 while connecting or connected */
    char to[64];            /* empty: not used */
    const char *tag;        /* "ble", "zigbee": log prefix and the hello line's stream name */
    bool connecting;
    uint64_t next_try, backoff;
    int last_error;
};
void uplink_init(struct uplink *u, size_t cap, const char *to, const char *tag);
bool uplink_up(const struct uplink *u);                 /* connected and hello sent */
void uplink_tick(struct uplink *u, uint64_t now);       /* starts a connect when due */
/* Poll events: POLLOUT while connecting, else POLLIN plus POLLOUT when output is queued. */
short uplink_events(const struct uplink *u);
/* Call with the revents while connecting; true when the connection just came up. */
bool uplink_connected(struct uplink *u, short re, uint64_t now);
void uplink_close(struct uplink *u, uint64_t now, const char *why, bool again_soon);

/* ---- poll set ---- */
#define POLLSET_MAX 48
struct pollset {
    struct pollfd fds[POLLSET_MAX];
    int n;
};
void pollset_reset(struct pollset *ps);
int pollset_add(struct pollset *ps, int fd, short events); /* returns index or -1 */
short pollset_revents(const struct pollset *ps, int idx);  /* 0 when idx < 0 */

/* ---- cpcd.c: child supervision ---- */
void cpcd_configure(const struct config *c);
void cpcd_tick(uint64_t now);
void cpcd_stop(void);
/* Terminate cpcd and start it again at once. cpcd's start-up reset sequence
 * reboots the whole NCP, which is the only way to get the Zigbee stack back to
 * its power-on state (the EZSP layer has no reset command). */
void cpcd_restart(const char *why);
bool cpcd_restarting(void); /* a requested restart is in progress: link loss is expected */
pid_t cpcd_pid(void);

/* ---- link.c: libcpc connection to cpcd ---- */
void link_tick(uint64_t now);
bool link_up(void);
cpc_handle_t link_handle(void);
unsigned link_generation(void);   /* increments every time the link comes up */
void link_request_down(const char *why); /* a module saw ECONNRESET etc. */
void link_shutdown(void);

/* ---- ble.c ---- */
int ble_init(const struct config *c);
void ble_tick(uint64_t now);
void ble_poll_add(struct pollset *ps);
void ble_poll_handle(const struct pollset *ps, uint64_t now);
void ble_link_down(void);
void ble_shutdown(void);

/* ---- api.c: ESPHome native API (Bluetooth proxy) ---- */
int api_init(const struct config *c); /* 0 also when api_port is 0 (off) */
bool api_enabled(void);
void api_advert(uint64_t now, const uint8_t addr[6], uint8_t addr_type, int8_t rssi, const uint8_t *data, size_t len);
void api_tick(uint64_t now);
void api_poll_add(struct pollset *ps);
void api_poll_handle(const struct pollset *ps, uint64_t now);
void api_shutdown(void);
void api_log_stats(void);

/* ---- mdns.c ---- */
int mdns_init(const struct config *c);
void mdns_tick(uint64_t now);
void mdns_poll_add(struct pollset *ps);
void mdns_poll_handle(const struct pollset *ps);
void mdns_shutdown(void);

/* ---- zb.c ---- */
int zb_init(const struct config *c);
void zb_tick(uint64_t now);
void zb_poll_add(struct pollset *ps);
void zb_poll_handle(const struct pollset *ps, uint64_t now);
void zb_link_down(void);
void zb_shutdown(void);

#endif
