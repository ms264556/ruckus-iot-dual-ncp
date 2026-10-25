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

/* ---- config.c ---- */
struct config {
    bool ble_enabled;
    bool zigbee_enabled;
    int ble_port;
    int zigbee_port;
    char cpcd_path[256];
    char cpcd_conf[256];
};
void config_defaults(struct config *c);
int config_load(struct config *c, const char *path); /* 0 ok, -1 unreadable */

/* ---- net.c ---- */
int net_listen(int port);           /* non-blocking listening socket, or -1 */
int net_accept(int listen_fd);      /* non-blocking accepted socket, or -1 */

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

/* ---- zb.c ---- */
int zb_init(const struct config *c);
void zb_tick(uint64_t now);
void zb_poll_add(struct pollset *ps);
void zb_poll_handle(const struct pollset *ps, uint64_t now);
void zb_link_down(void);
void zb_shutdown(void);

#endif
