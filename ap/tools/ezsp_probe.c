/* ezsp_probe: check the Zigbee NCP on CPC endpoint 5 directly through cpcd.
 *
 * Sends the legacy-format EZSP version command (the first command must use the
 * legacy 3-byte header), then getEui64 and networkState in the extended format,
 * and prints the replies. Stop iotd first: an endpoint has one client at a time.
 *
 * libcpc rejects cpc_read_endpoint() with a buffer smaller than
 * SL_CPC_READ_MINIMUM_SIZE (4087 bytes).
 *
 * Run (AP): ezsp_probe [cpcd_instance [desired_ezsp_version]]     defaults: cpcd_0 0x0D
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sl_cpc.h"

static cpc_endpoint_t ep;
static uint8_t rxbuf[SL_CPC_READ_MINIMUM_SIZE];

static void dump(const char *tag, const uint8_t *b, ssize_t n)
{
    printf("%s %zd:", tag, n);
    for (ssize_t k = 0; k < n; k++) {
        printf(" %02X", b[k]);
    }
    printf("\n");
    fflush(stdout);
}

/* Write one EZSP frame, poll for the reply for wait_ms. Returns reply length, 0 on timeout, -1 on error. */
static int xfer(const char *name, const uint8_t *f, size_t n, int wait_ms)
{
    printf("== %s\n", name);
    dump("   tx", f, (ssize_t)n);
    ssize_t w = cpc_write_endpoint(ep, f, n, CPC_ENDPOINT_WRITE_FLAG_NONE);
    if (w != (ssize_t)n) {
        printf("   write failed: %zd\n", w);
        fflush(stdout);
        return -1;
    }
    for (int i = 0; i < wait_ms / 10; i++) {
        ssize_t r = cpc_read_endpoint(ep, rxbuf, sizeof(rxbuf), CPC_ENDPOINT_READ_FLAG_NON_BLOCKING);
        if (r > 0) {
            dump("   rx", rxbuf, r);
            return (int)r;
        }
        if (r < 0 && -r != EAGAIN && -r != EWOULDBLOCK) {
            printf("   read error %zd\n", r);
            fflush(stdout);
            return -1;
        }
        usleep(10000);
    }
    printf("   no reply in %d ms\n", wait_ms);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    cpc_handle_t h;
    const char *inst = argc > 1 ? argv[1] : "cpcd_0";
    if (cpc_init(&h, inst, false, NULL) != 0) {
        printf("cpc_init(%s) failed\n", inst);
        return 1;
    }
    int fd = -1;
    for (int i = 0; i < 10 && fd < 0; i++) {
        fd = cpc_open_endpoint(h, &ep, 5, 1);
        if (fd < 0) {
            sleep(1);
        }
    }
    if (fd < 0) {
        printf("endpoint 5 open failed: %d\n", fd);
        return 1;
    }
    printf("endpoint 5 open fd=%d, read buffer %u bytes\n", fd, (unsigned)sizeof(rxbuf));

    /* 1. version, legacy format: seq, frameControl, frameId=0x00, desiredProtocolVersion (0x0D = v13, EmberZNet 7.4).
     * An NCP speaking another version answers with its own; the host must then repeat the command with that
     * version before anything else is accepted. */
    uint8_t desired = argc > 2 ? (uint8_t)strtoul(argv[2], NULL, 0) : 0x0D;
    uint8_t ver[] = { 0x00, 0x00, 0x00, desired };
    int r = xfer("version (legacy frame)", ver, sizeof(ver), 3000);
    if (r >= 7) {
        printf("   -> EZSP protocol v%u, stack type 0x%02X, stack version 0x%04X\n",
               rxbuf[3], rxbuf[4], rxbuf[5] | (rxbuf[6] << 8));
        if (rxbuf[3] != desired) {
            ver[0] = 0x03;
            ver[3] = rxbuf[3];
            r = xfer("version again, with the NCP's version", ver, sizeof(ver), 3000);
        }
    }
    if (r > 0) {
        /* 2. getEui64 (0x0026), extended format: seq, fc low, fc high (format version 1), id low, id high */
        uint8_t eui[] = { 0x01, 0x00, 0x01, 0x26, 0x00 };
        r = xfer("getEui64 (extended frame)", eui, sizeof(eui), 3000);
        if (r >= 13) {
            printf("   -> EUI64");
            for (int k = 12; k >= 5; k--) {
                printf(" %02X", rxbuf[k]);
            }
            printf("\n");
        }
        /* 3. networkState (0x0018) */
        uint8_t ns[] = { 0x02, 0x00, 0x01, 0x18, 0x00 };
        r = xfer("networkState (extended frame)", ns, sizeof(ns), 3000);
        if (r >= 6) {
            printf("   -> network state 0x%02X (0 = no network)\n", rxbuf[5]);
        }
    }
    cpc_close_endpoint(&ep);
    printf("done\n");
    return 0;
}
