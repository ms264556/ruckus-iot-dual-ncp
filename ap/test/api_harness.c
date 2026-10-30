/* Runs iotd's ESPHome API server (api.c) on a host without the radio, for test/esphome_client.py:
 *
 *   make -C ap test/api_harness && ap/test/api_harness <port> <base64 key>
 *
 * Feeds it 20 made-up advertisements a second from 5 addresses. A third argument "mdns" also runs the
 * mDNS responder (mdns.c).
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "iotd.h"

static volatile sig_atomic_t stop_flag;
static void on_signal(int sig) { (void)sig; stop_flag = 1; }

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: api_harness <port> <base64 key> [mdns]\n");
        return 2;
    }
    char conf[256];
    snprintf(conf, sizeof(conf), "/tmp/api_harness.%d.conf", (int)getpid());
    FILE *f = fopen(conf, "w");
    fprintf(f, "api_port = %s\napi_key = %s\napi_name = harness-112233\napi_friendly_name = Test Harness\n"
               "api_mac = AA:BB:CC:11:22:33\napi_model = Ruckus R560\napi_bluetooth_mac = F4:B3:B1:E8:0A:CC\napi_mdns = %s\n",
            argv[1], argv[2], argc == 4 ? "yes" : "no");
    fclose(f);
    struct config cfg;
    config_defaults(&cfg);
    if (config_load(&cfg, conf) < 0 || api_init(&cfg) < 0 || mdns_init(&cfg) < 0) {
        return 1;
    }
    remove(conf);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    struct pollset ps;
    uint64_t next = 0;
    unsigned seq = 0;
    while (!stop_flag) {
        uint64_t now = now_ms();
        if (now >= next) {
            next = now + 50;
            uint8_t addr[6] = { (uint8_t)(seq % 5), 0x59, 0x9f, 0xaa, 0x32, 0x05 };
            uint8_t data[31] = { 0x02, 0x01, 0x06, 0x1a, 0xff, 0x4c, 0x00 };
            api_advert(now, addr, 1, (int8_t)(-40 - (int)(seq % 30)), data, sizeof(data));
            seq++;
        }
        api_tick(now);
        mdns_tick(now);
        pollset_reset(&ps);
        api_poll_add(&ps);
        mdns_poll_add(&ps);
        poll(ps.fds, (nfds_t)ps.n, 20);
        api_poll_handle(&ps, now_ms());
        mdns_poll_handle(&ps);
    }
    mdns_shutdown();
    api_shutdown();
    return 0;
}
