/* iotd: supervise cpcd, serve BLE advertisements and Zigbee EZSP/ASH over TCP. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "iotd.h"

static volatile sig_atomic_t stop_flag;

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: iotd [-v] [-c config]\n"
            "  -v         debug logging (frames, retries)\n"
            "  -c config  key = value file, default " IOTD_DEFAULT_CONF "\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *conf_path = IOTD_DEFAULT_CONF;
    bool conf_given = false;
    int opt;
    while ((opt = getopt(argc, argv, "vc:h")) != -1) {
        switch (opt) {
        case 'v':
            log_set_verbose(true);
            break;
        case 'c':
            conf_path = optarg;
            conf_given = true;
            break;
        default:
            usage();
        }
    }
    if (optind != argc) {
        usage();
    }

    struct config cfg;
    config_defaults(&cfg);
    if (config_load(&cfg, conf_path) < 0) {
        if (conf_given) {
            LOGE("cannot read %s: %s", conf_path, strerror(errno));
            return 1;
        }
        LOGW("no %s, using defaults", conf_path);
    }
    LOGI("iotd starting: ble %s (port %d, connect %s, esphome api %d), zigbee %s (port %d, %d allowed, connect %s), cpcd %s -c %s",
         cfg.ble_enabled ? "on" : "off", cfg.ble_port, cfg.ble_connect[0] ? cfg.ble_connect : "none", cfg.api_port,
         cfg.zigbee_enabled ? "on" : "off", cfg.zigbee_port, cfg.zigbee_allow.n,
         cfg.zigbee_connect[0] ? cfg.zigbee_connect : "none", cfg.cpcd_path, cfg.cpcd_conf);
    if (!cfg.ble_enabled && !cfg.zigbee_enabled) {
        LOGE("both radios disabled, nothing to do");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if (cfg.ble_enabled && ble_init(&cfg) < 0) {
        return 1;
    }
    /* The ESPHome proxy needs the Bluetooth radio. */
    if (cfg.ble_enabled && (api_init(&cfg) < 0 || mdns_init(&cfg) < 0)) {
        return 1;
    }
    if (cfg.zigbee_enabled && zb_init(&cfg) < 0) {
        return 1;
    }
    cpcd_configure(&cfg);

    struct pollset ps;
    while (!stop_flag) {
        uint64_t now = now_ms();
        cpcd_tick(now);
        link_tick(now);
        if (cfg.ble_enabled) {
            ble_tick(now);
            api_tick(now);
            mdns_tick(now);
        }
        if (cfg.zigbee_enabled) {
            zb_tick(now);
        }
        pollset_reset(&ps);
        if (cfg.ble_enabled) {
            ble_poll_add(&ps);
            api_poll_add(&ps);
            mdns_poll_add(&ps);
        }
        if (cfg.zigbee_enabled) {
            zb_poll_add(&ps);
        }
        int r = poll(ps.fds, (nfds_t)ps.n, 100);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOGE("poll: %s", strerror(errno));
            break;
        }
        now = now_ms();
        if (cfg.ble_enabled) {
            ble_poll_handle(&ps, now);
            api_poll_handle(&ps, now);
            mdns_poll_handle(&ps);
        }
        if (cfg.zigbee_enabled) {
            zb_poll_handle(&ps, now);
        }
    }

    LOGI("shutting down");
    if (cfg.zigbee_enabled) {
        zb_shutdown();
    }
    if (cfg.ble_enabled) {
        mdns_shutdown();
        api_shutdown();
        ble_shutdown();
    }
    link_shutdown();
    cpcd_stop();
    LOGI("stopped");
    return 0;
}
