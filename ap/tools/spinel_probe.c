/* spinel_probe: ask the AP's IoT radio, when it runs an OpenThread RCP image, for its version and EUI-64.
 *
 *   spinel_probe [uart]        uart defaults to /dev/ttyMSM1
 *
 * Speaks Spinel in HDLC-lite framing at 115200 with RTS/CTS (the ot-rcp images): PROP_VALUE_GET NCP_VERSION and
 * HWADDR, printing
 *
 *   -> Spinel version SL-OPENTHREAD/2.4.0.0_GitHub-7074a43e4; EFR32; Nov  1 2026 21:46:37
 *   -> EUI64 F4 B3 B1 FF FE E8 0A CC
 *
 * and exits 0, or exits 1 when the radio does not answer (another image, or the bootloader). Nothing else may hold
 * the UART while it runs; it sends no reset.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#define PROP_NCP_VERSION 0x02
#define PROP_HWADDR      0x08
#define CMD_VALUE_GET    0x02
#define CMD_VALUE_IS     0x06

static long ms_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static uint16_t fcs16(const uint8_t *p, size_t n)
{
    uint16_t fcs = 0xFFFF; /* CRC-16/X.25 */
    for (size_t i = 0; i < n; i++) {
        fcs ^= p[i];
        for (int b = 0; b < 8; b++) {
            fcs = (fcs & 1) ? (uint16_t)((fcs >> 1) ^ 0x8408) : (uint16_t)(fcs >> 1);
        }
    }
    return fcs ^ 0xFFFF;
}

static int send_frame(int fd, const uint8_t *payload, size_t n)
{
    uint8_t body[64], out[132];
    if (n + 2 > sizeof(body)) {
        return -1;
    }
    memcpy(body, payload, n);
    uint16_t fcs = fcs16(payload, n);
    body[n] = (uint8_t)(fcs & 0xFF);
    body[n + 1] = (uint8_t)(fcs >> 8);
    size_t m = 0;
    out[m++] = 0x7E;
    for (size_t i = 0; i < n + 2; i++) {
        uint8_t c = body[i];
        if (c == 0x7E || c == 0x7D || c == 0x11 || c == 0x13 || c == 0xF8) {
            out[m++] = 0x7D;
            c ^= 0x20;
        }
        out[m++] = c;
    }
    out[m++] = 0x7E;
    return write(fd, out, m) == (ssize_t)m ? 0 : -1;
}

/* The next good frame (payload without FCS) within timeout_ms; its length, or -1. */
static int read_frame(int fd, uint8_t *buf, size_t cap, int timeout_ms)
{
    long deadline = ms_now() + timeout_ms;
    size_t n = 0;
    int escaped = 0;
    while (ms_now() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { 0, 50000 };
        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        uint8_t c;
        if (read(fd, &c, 1) != 1) {
            continue;
        }
        if (c == 0x7E) {
            if (n >= 3 && fcs16(buf, n - 2) == (uint16_t)(buf[n - 2] | (buf[n - 1] << 8))) {
                return (int)(n - 2);
            }
            n = 0;
            escaped = 0;
            continue;
        }
        if (c == 0x7D) {
            escaped = 1;
            continue;
        }
        if (escaped) {
            c ^= 0x20;
            escaped = 0;
        }
        if (n < cap) {
            buf[n++] = c;
        } else {
            n = 0;
        }
    }
    return -1;
}

/* PROP_VALUE_GET <prop>; copies the value into val and returns its length, or -1. */
static int get_prop(int fd, uint8_t tid, uint8_t prop, uint8_t *val, size_t cap)
{
    const uint8_t req[] = { (uint8_t)(0x80 | tid), CMD_VALUE_GET, prop };
    if (send_frame(fd, req, sizeof(req)) != 0) {
        return -1;
    }
    long deadline = ms_now() + 1500;
    uint8_t buf[600];
    while (ms_now() < deadline) {
        int n = read_frame(fd, buf, sizeof(buf), (int)(deadline - ms_now()));
        if (n < 3) {
            continue;
        }
        /* skip unsolicited frames (logs, TID 0) and anything not answering this property */
        if ((buf[0] & 0x0F) != tid || buf[1] != CMD_VALUE_IS || buf[2] != prop) {
            continue;
        }
        size_t len = (size_t)n - 3 < cap ? (size_t)n - 3 : cap;
        memcpy(val, buf + 3, len);
        return (int)len;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *uart = argc > 1 ? argv[1] : "/dev/ttyMSM1";
    int fd = open(uart, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", uart, strerror(errno));
        return 2;
    }
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        perror("tcgetattr");
        return 2;
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= CLOCAL | CREAD | CRTSCTS;
    tio.c_cflag &= ~CSTOPB;
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);

    uint8_t val[256];
    int n = get_prop(fd, 1, PROP_NCP_VERSION, val, sizeof(val) - 1);
    if (n < 0) {
        n = get_prop(fd, 2, PROP_NCP_VERSION, val, sizeof(val) - 1); /* once more: the first frame may meet a busy RCP */
    }
    if (n < 0) {
        fprintf(stderr, "no Spinel answer on %s\n", uart);
        return 1;
    }
    val[n] = 0;
    printf("-> Spinel version %s\n", (char *)val);
    n = get_prop(fd, 3, PROP_HWADDR, val, sizeof(val));
    if (n == 8) {
        printf("-> EUI64 %02X %02X %02X %02X %02X %02X %02X %02X\n", val[0], val[1], val[2], val[3], val[4], val[5], val[6],
               val[7]);
    }
    close(fd);
    return 0;
}
