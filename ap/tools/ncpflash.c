/* ncpflash: flash a GBL image onto the AP's built-in IoT radio through its Gecko bootloader.
 *
 *   ncpflash [--enter pins|ezsp|none] <file.gbl> [uart]        uart defaults to /dev/ttyMSM1
 *
 * Gets the radio to its bootloader menu, uploads over XMODEM-CRC at 115200, then runs the
 * new application. Nothing else may hold the UART while it runs. A radio already at the
 * bootloader prompt (an interrupted upload, or an image that rebooted into its bootloader)
 * is used as it is. Otherwise --enter says how to get there:
 *
 *   pins  (default) the reset and boot-select lines, /proc/v54bsp/iot_rst and iot_rcv
 *         (EFR32MG21 radios: R560 and its relatives)
 *   ezsp  ask the running Zigbee NCP over ASH: version, then launchStandaloneBootloader.
 *         For radios without a hardware bootloader entry (the R730's EFR32MG12) that run an
 *         EZSP image over plain ASH (the factory and vendor Zigbee images). A radio running
 *         a CPC image is sent to its bootloader with cpcd --enter-bootloader instead, then
 *         flashed with --enter none.
 *   none  the radio must already be at the prompt
 *
 * Only pins touches iot_rcv; ezsp may pulse iot_rst, which restarts the application.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <signal.h>

#include "ash_codec.h"

#define SOH 0x01
#define EOT 0x04
#define ACK 0x06
#define NAK 0x15
#define CAN 0x18
#define CRC_REQ 0x43

static void on_alarm(int sig)
{
    (void)sig;
    const char msg[] = "\nncpflash: timeout\n";
    write(2, msg, sizeof(msg) - 1);
    _exit(3);
}

static long ms_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static int gpio_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%s\n", val);
    ssize_t n = write(fd, buf, strlen(buf));
    close(fd);
    if (n != (ssize_t)strlen(buf)) {
        fprintf(stderr, "write %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static void enter_bootloader(void)
{
    fprintf(stderr, "gpio: rst low\n");
    gpio_write("/proc/v54bsp/iot_rst", "0");
    usleep(300000);
    fprintf(stderr, "gpio: rcv low\n");
    gpio_write("/proc/v54bsp/iot_rcv", "0");
    usleep(300000);
    fprintf(stderr, "gpio: rst high\n");
    gpio_write("/proc/v54bsp/iot_rst", "1");
    usleep(300000);
    fprintf(stderr, "gpio: done\n");
}

static void release_run(void)
{
    gpio_write("/proc/v54bsp/iot_rcv", "1");
    usleep(100000);
    gpio_write("/proc/v54bsp/iot_rst", "0");
    usleep(200000);
    gpio_write("/proc/v54bsp/iot_rst", "1");
    usleep(100000);
}

static void reset_pulse(void)
{
    fprintf(stderr, "gpio: rst pulse\n");
    gpio_write("/proc/v54bsp/iot_rst", "0");
    usleep(300000);
    gpio_write("/proc/v54bsp/iot_rst", "1");
}

static int serial_open(const char *dev, int baud)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror(dev);
        return -1;
    }
    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    if (tcgetattr(fd, &tio) != 0) {
        perror("tcgetattr");
    }
    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    speed_t sp = (baud == 921600) ? B921600 : B115200;
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int read_byte(int fd, int timeout_ms)
{
    long deadline = ms_now() + timeout_ms;
    while (ms_now() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { 0, 50000 };
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) {
            unsigned char c;
            ssize_t n = read(fd, &c, 1);
            if (n == 1) {
                return c;
            }
        }
    }
    return -1;
}

static int read_until(int fd, const char *needle, int timeout_ms)
{
    long deadline = ms_now() + timeout_ms;
    size_t matched = 0;
    size_t len = strlen(needle);
    while (ms_now() < deadline) {
        int c = read_byte(fd, 100);
        if (c < 0) {
            continue;
        }
        if (c >= 32 || c == '\n' || c == '\r') {
            fputc(c, stderr);
        }
        if (c == (unsigned char)needle[matched]) {
            if (++matched == len) {
                return 0;
            }
        } else {
            matched = (c == (unsigned char)needle[0]) ? 1 : 0;
        }
    }
    return -1;
}

/* The bootloader prompt within timeout_ms, nudging it with a newline halfway. */
static int wait_prompt(int fd, int timeout_ms)
{
    if (read_until(fd, "BL >", timeout_ms / 2) == 0) {
        return 0;
    }
    if (write(fd, "\r\n", 2) != 2) {
        return -1;
    }
    return read_until(fd, "BL >", timeout_ms / 2);
}

/* ---- ASH/EZSP: just enough host to launch the standalone bootloader ---------------- */

static uint8_t ash_frm; /* our next frame number */
static uint8_t ash_ack; /* next frame number expected from the NCP */

static int ash_send(int fd, const uint8_t *frame, size_t n)
{
    uint8_t out[ASH_MAX_ENCODED];
    size_t m = ash_encode(frame, n, out, sizeof(out));
    return (m && write(fd, out, m) == (ssize_t)m) ? 0 : -1;
}

/* Waits for a frame; returns its length (control byte first, in d->buf), or -1 on timeout. */
static int ash_read_frame(int fd, struct ash_decoder *d, int timeout_ms)
{
    long deadline = ms_now() + timeout_ms;
    while (ms_now() < deadline) {
        int c = read_byte(fd, 100);
        if (c < 0) {
            continue;
        }
        int r = ash_decode_byte(d, (uint8_t)c);
        if (r > 0) {
            return r;
        }
    }
    return -1;
}

static int ash_reset(int fd, struct ash_decoder *d)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt == 3) {
            reset_pulse(); /* restarts the application, which announces itself with RSTACK */
        } else {
            uint8_t can = ASH_CAN;
            uint8_t rst = ASH_CONTROL_RST;
            if (write(fd, &can, 1) != 1 || ash_send(fd, &rst, 1) != 0) {
                return -1;
            }
        }
        ash_decoder_reset(d);
        long deadline = ms_now() + 3000;
        while (ms_now() < deadline) {
            int n = ash_read_frame(fd, d, (int)(deadline - ms_now()));
            if (n >= 3 && d->buf[0] == ASH_CONTROL_RSTACK) {
                fprintf(stderr, "ash: RSTACK version %u reset code 0x%02X\n", d->buf[1], d->buf[2]);
                ash_frm = ash_ack = 0;
                return 0;
            }
        }
    }
    fprintf(stderr, "ash: no RSTACK\n");
    return -1;
}

/* Sends one EZSP frame and returns the length of the NCP's reply (de-randomized, in reply), or -1. */
static int ezsp_call(int fd, struct ash_decoder *d, const uint8_t *ezsp, size_t n, uint8_t *reply, size_t cap)
{
    uint8_t frame[1 + ASH_MAX_DATA];
    if (n > ASH_MAX_DATA) {
        return -1;
    }
    for (int tries = 0; tries < 3; tries++) {
        frame[0] = (uint8_t)((ash_frm << 4) | (tries ? 0x08 : 0x00) | ash_ack);
        memcpy(frame + 1, ezsp, n);
        ash_randomize(frame + 1, n);
        if (ash_send(fd, frame, n + 1) != 0) {
            return -1;
        }
        long deadline = ms_now() + 2000;
        while (ms_now() < deadline) {
            int len = ash_read_frame(fd, d, (int)(deadline - ms_now()));
            if (len < 1) {
                break;
            }
            uint8_t control = d->buf[0];
            if (control == ASH_CONTROL_ERROR || control == ASH_CONTROL_RSTACK) {
                fprintf(stderr, "ash: NCP sent 0x%02X instead of a reply\n", control);
                return -1;
            }
            if ((control & 0x80) == 0) { /* DATA */
                ash_ack = (uint8_t)(((control >> 4) + 1) & 7);
                uint8_t ack = (uint8_t)(ASH_CONTROL_ACK | ash_ack);
                ash_send(fd, &ack, 1);
                size_t m = (size_t)len - 1;
                if (m > cap) {
                    m = cap;
                }
                memcpy(reply, d->buf + 1, m);
                ash_randomize(reply, m);
                ash_frm = (uint8_t)((ash_frm + 1) & 7);
                return (int)m;
            }
            if ((control & 0xE0) == ASH_CONTROL_NAK) {
                break; /* retransmit */
            }
        }
    }
    fprintf(stderr, "ezsp: no reply\n");
    return -1;
}

/* version (always in the legacy frame format) -> the NCP's protocol version, or -1 */
static int ezsp_version(int fd, struct ash_decoder *d, uint8_t seq, uint8_t desired)
{
    uint8_t cmd[] = { seq, 0x00, 0x00, desired };
    uint8_t reply[32];
    int n = ezsp_call(fd, d, cmd, sizeof(cmd), reply, sizeof(reply));
    /* legacy [seq, fc, 0x00, params] or the EZSP 6/7 extended form [seq, fc, 0xFF, xfc, 0x00, params] */
    int p = (n >= 3 && reply[2] == 0xFF) ? 5 : 3;
    if (n < p + 4 || reply[p - 1] != 0x00) {
        fprintf(stderr, "ezsp: unexpected version reply (%d bytes):", n);
        for (int i = 0; i < n; i++) {
            fprintf(stderr, " %02X", reply[i]);
        }
        fprintf(stderr, "\n");
        return -1;
    }
    fprintf(stderr, "ezsp: protocol %u, stack type %u, stack version 0x%02X%02X\n",
            reply[p], reply[p + 1], reply[p + 3], reply[p + 2]);
    return reply[p];
}

static int enter_bootloader_ezsp(int fd)
{
    struct ash_decoder d;
    ash_decoder_reset(&d);
    if (ash_reset(fd, &d) != 0) {
        return -1;
    }
    /* The NCP answers a version command with its own protocol, but it keeps working in the protocol the host asked for
     * only when they match: the R730's factory EmberZNet 5.10 (protocol 5) asked for 4 answers later commands with a
     * bare "FF 00" and ignores the launch, and answers a second version command in a layout of its own. So learn the
     * protocol, then start a fresh ASH session whose first command names it, as a host that knows the NCP would. */
    int proto = ezsp_version(fd, &d, 0, 4);
    if (proto > 0 && proto != 4) {
        usleep(250000);
        if (ash_reset(fd, &d) != 0) {
            return -1;
        }
        int again = ezsp_version(fd, &d, 0, (uint8_t)proto);
        if (again != proto) {
            fprintf(stderr, "ezsp: the NCP answered protocol %d, then %d\n", proto, again);
            return -1;
        }
    }
    if (proto < 4) {
        return -1;
    }
    usleep(1500000); /* this NCP ignores (NAKs) a frame that follows its reply too closely; 900 ms after the ACK worked, 250 ms is untested */
    /* launchStandaloneBootloader (0x8F), mode 1 = STANDALONE_BOOTLOADER_NORMAL_MODE; protocol 8
     * and later only take the extended frame format. Sent exactly once and never retransmitted:
     * the NCP resets into the bootloader, whose menu would take stray bytes as commands. */
    uint8_t legacy[] = { 1, 0x00, 0x8F, 0x01 };
    uint8_t extended[] = { 1, 0x00, 0x01, 0x8F, 0x00, 0x01 };
    const uint8_t *cmd = proto >= 8 ? extended : legacy;
    size_t n = proto >= 8 ? sizeof(extended) : sizeof(legacy);
    uint8_t frame[1 + sizeof(extended)];
    frame[0] = (uint8_t)((ash_frm << 4) | ash_ack);
    memcpy(frame + 1, cmd, n);
    ash_randomize(frame + 1, n);
    fprintf(stderr, "ezsp: launchStandaloneBootloader\n");
    if (ash_send(fd, frame, n + 1) != 0) {
        return -1;
    }
    /* The NCP resets only once its reply is acknowledged (until then it retransmits it), so ACK every DATA frame it
     * sends. An ACK only ever answers a received frame: once the radio is in its bootloader nothing more is sent. */
    long deadline = ms_now() + 4000;
    while (ms_now() < deadline) {
        int len = ash_read_frame(fd, &d, 300);
        if (len < 1) {
            continue;
        }
        uint8_t control = d.buf[0];
        if ((control & 0x80) == 0) {
            ash_ack = (uint8_t)(((control >> 4) + 1) & 7);
            uint8_t ack = (uint8_t)(ASH_CONTROL_ACK | ash_ack);
            ash_send(fd, &ack, 1);
            if (len > 1) {
                uint8_t reply[16];
                size_t m = (size_t)len - 1 < sizeof(reply) ? (size_t)len - 1 : sizeof(reply);
                memcpy(reply, d.buf + 1, m);
                ash_randomize(reply, m);
                fprintf(stderr, "ezsp: reply");
                for (size_t i = 0; i < m; i++) {
                    fprintf(stderr, " %02X", reply[i]);
                }
                fprintf(stderr, " (acknowledged)\n");
            }
        }
    }
    return 0;
}

/* ---- XMODEM ----------------------------------------------------------------------- */

static uint16_t crc16(const unsigned char *p, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static int xmodem_send(int fd, const unsigned char *data, size_t len)
{
    size_t off = 0;
    unsigned char seq = 1;
    int c = -1;

    long deadline = ms_now() + 15000;
    while (ms_now() < deadline) {
        c = read_byte(fd, 200);
        if (c == CRC_REQ || c == NAK) {
            break;
        }
    }
    if (c != CRC_REQ && c != NAK) {
        fprintf(stderr, "no XMODEM start byte (got %d)\n", c);
        return -1;
    }

    while (off < len) {
        unsigned char pkt[3 + 128 + 2];
        size_t n = len - off < 128 ? len - off : 128;
        pkt[0] = SOH;
        pkt[1] = seq;
        pkt[2] = (unsigned char)~seq;
        memset(pkt + 3, 0x1A, 128);
        memcpy(pkt + 3, data + off, n);
        uint16_t crc = crc16(pkt + 3, 128);
        pkt[131] = (unsigned char)(crc >> 8);
        pkt[132] = (unsigned char)(crc & 0xFF);

        int ok = 0;
        for (int retries = 0; retries < 20 && !ok; retries++) {
            if (write(fd, pkt, sizeof(pkt)) != (ssize_t)sizeof(pkt)) {
                continue;
            }
            tcdrain(fd); /* the whole packet is on the wire before the reply window starts (some kernels buffer deeply) */
            long dl = ms_now() + 3000;
            int r = -1;
            do {
                r = read_byte(fd, 100);
                if (r == ACK || r == NAK || r == CAN) {
                    break;
                }
            } while (ms_now() < dl);
            if (r == ACK) {
                ok = 1;
            } else if (r == CAN) {
                fprintf(stderr, "receiver cancelled\n");
                return -1;
            } else {
                fprintf(stderr, "\nblock %u at %zu: %s, retry %d\n", seq, off, r == NAK ? "NAK" : "no reply", retries + 1);
            }
        }
        if (!ok) {
            fprintf(stderr, "block %u failed\n", seq);
            return -1;
        }
        off += n;
        seq++;
        if ((off / 128) % 64 == 0) {
            fprintf(stderr, "\r%zu/%zu bytes", off, len);
        }
    }
    fprintf(stderr, "\r%zu/%zu bytes\n", off, len);

    for (int i = 0; i < 10; i++) {
        if (write(fd, "\x04", 1) != 1) {
            continue;
        }
        int r = read_byte(fd, 2000);
        if (r == ACK) {
            return 0;
        }
    }
    fprintf(stderr, "EOT not acknowledged\n");
    return -1;
}

int main(int argc, char **argv)
{
    const char *enter = "pins";
    int arg = 1;
    if (argc > 2 && strcmp(argv[1], "--enter") == 0) {
        enter = argv[2];
        arg = 3;
    }
    if (argc <= arg || (strcmp(enter, "pins") && strcmp(enter, "ezsp") && strcmp(enter, "none"))) {
        fprintf(stderr, "usage: %s [--enter pins|ezsp|none] <file.gbl> [uart]\n", argv[0]);
        return 2;
    }
    const char *image = argv[arg];
    const char *uart = argc > arg + 1 ? argv[arg + 1] : "/dev/ttyMSM1";
    int pins = strcmp(enter, "pins") == 0;
    signal(SIGALRM, on_alarm);
    alarm(240);

    struct stat st;
    if (stat(image, &st) != 0) {
        perror(image);
        return 2;
    }
    FILE *f = fopen(image, "rb");
    if (!f) {
        perror(image);
        return 2;
    }
    unsigned char *buf = malloc(st.st_size);
    if (!buf || fread(buf, 1, st.st_size, f) != (size_t)st.st_size) {
        fprintf(stderr, "read failed\n");
        return 2;
    }
    fclose(f);
    fprintf(stderr, "image %s: %ld bytes\n", image, (long)st.st_size);

    fprintf(stderr, "uart %s\n", uart);
    int fd = serial_open(uart, 115200);
    if (fd < 0) {
        return 1;
    }

    fprintf(stderr, "looking for a bootloader prompt\n");
    if (write(fd, "\r\n", 2) == 2 && read_until(fd, "BL >", 1500) == 0) {
        fprintf(stderr, "\nalready at the bootloader prompt\n");
    } else if (strcmp(enter, "none") == 0) {
        fprintf(stderr, "\nno bootloader prompt\n");
        return 1;
    } else {
        fprintf(stderr, "entering bootloader (%s)\n", enter);
        if (pins) {
            enter_bootloader();
        } else if (enter_bootloader_ezsp(fd) != 0) {
            fprintf(stderr, "could not ask the NCP for its bootloader\n");
            return 1;
        }
        if (wait_prompt(fd, pins ? 6000 : 10000) != 0) {
            fprintf(stderr, "\nno bootloader prompt\n");
            return 1;
        }
    }
    fprintf(stderr, "\nbootloader prompt seen, selecting upload\n");
    if (write(fd, "1\r\n", 3) != 3) {
        fprintf(stderr, "write menu selection failed\n");
    }
    usleep(300000);

    if (xmodem_send(fd, buf, st.st_size) != 0) {
        return 1;
    }
    fprintf(stderr, "\nupload complete, running\n");
    usleep(500000);
    if (write(fd, "2\r\n", 3) != 3) {
        fprintf(stderr, "write run failed\n");
    }
    usleep(300000);

    close(fd);
    if (pins) {
        release_run();
    }
    fprintf(stderr, "done\n");
    return 0;
}
