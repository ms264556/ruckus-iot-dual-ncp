/* ncpflash: flash a GBL image onto the AP's built-in IoT radio through its Gecko bootloader.
 *
 *   ncpflash [--enter pins|ezsp|spinel|none] <file.gbl> [uart]        uart defaults to /dev/ttyMSM1
 *   ncpflash --enter spi|none <file.gbl> /dev/spidevB.C [cs,irq,rst,boot]   a radio on SPI (sysfs GPIO numbers,
 *                                                                        default 22,55,21,54: the R650's EFR32MG21)
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
 *   spinel  ask the running OpenThread RCP: Spinel CMD_RESET with RESET_BOOTLOADER in HDLC-lite
 *         framing, RTS/CTS on (this package's ot-rcp images, built with the platform bootloader mode)
 *   none  the radio must already be at the prompt
 *   spi   a radio on SPI with the Gecko EZSP-SPI bootloader: its boot-select line held low through a reset pulse
 *         (the bootloader's GPIO activation; on the R650 the line the vendor calls nWAKE), then the bootloader's
 *         query handshake and XMODEM-CRC blocks inside 0xFD bootloader frames, at 500 kHz with a software chip
 *         select. With a /dev/spidev device, --enter none takes a radio already in that bootloader (after
 *         cpcd --enter-bootloader with the boot-select line low). The boot-select line is left high afterwards,
 *         so the application boots on every later reset, including the ones cpcd makes.
 *
 * Only pins touches iot_rcv; ezsp may pulse iot_rst, which restarts the application.
 *
 * The UART starts without RTS/CTS. An NCP that sends no RSTACK is tried again with them on (the R760's factory
 * MG21 is silent until RTS is asserted), and a bootloader that shows no prompt is nudged once with them switched
 * the other way.
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
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
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

/* RTS/CTS on the UART. Off by default (the R730's MG12 runs without); the R760's factory MG21 sends nothing until the
 * host asserts RTS, which this UART does only with hardware flow control on. */
static int flow_control;

static void set_flow_control(int fd, int on)
{
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        perror("tcgetattr");
        return;
    }
    if (on) {
        tio.c_cflag |= CRTSCTS;
    } else {
        tio.c_cflag &= ~CRTSCTS;
    }
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);
    flow_control = on;
    fprintf(stderr, "uart: RTS/CTS %s\n", on ? "on" : "off");
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
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt == 4) {
            if (flow_control) {
                break;
            }
            set_flow_control(fd, 1); /* nothing came back: an NCP that waits for RTS */
        }
        if (attempt % 4 == 3) {
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

/* ---- Spinel: one HDLC-lite frame asking an OpenThread RCP for its bootloader --------------------------------- */

static int enter_bootloader_spinel(int fd)
{
    /* header (flag, IID 0, TID 1), CMD_RESET, SPINEL_RESET_BOOTLOADER */
    const uint8_t payload[] = { 0x81, 0x01, 0x03 };
    uint16_t fcs = 0xFFFF; /* CRC-16/X.25, sent low byte first */
    for (size_t i = 0; i < sizeof(payload); i++) {
        fcs ^= payload[i];
        for (int b = 0; b < 8; b++) {
            fcs = (fcs & 1) ? (uint16_t)((fcs >> 1) ^ 0x8408) : (uint16_t)(fcs >> 1);
        }
    }
    fcs ^= 0xFFFF;
    uint8_t body[sizeof(payload) + 2], out[2 * sizeof(body) + 2];
    memcpy(body, payload, sizeof(payload));
    body[sizeof(payload)] = (uint8_t)(fcs & 0xFF);
    body[sizeof(payload) + 1] = (uint8_t)(fcs >> 8);
    size_t n = 0;
    out[n++] = 0x7E;
    for (size_t i = 0; i < sizeof(body); i++) {
        uint8_t c = body[i];
        if (c == 0x7E || c == 0x7D || c == 0x11 || c == 0x13 || c == 0xF8) {
            out[n++] = 0x7D;
            c ^= 0x20;
        }
        out[n++] = c;
    }
    out[n++] = 0x7E;
    if (!flow_control) {
        set_flow_control(fd, 1); /* the RCP image runs with hardware flow control */
    }
    fprintf(stderr, "spinel: RESET_BOOTLOADER\n");
    return write(fd, out, n) == (ssize_t)n ? 0 : -1;
}

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

/* ---- SPI: the Gecko EZSP-SPI bootloader (btl_ezsp_spi_common.c), for a radio on /dev/spidev --------------------- */

static int spi_fd = -1;
static int spi_cs = 22, spi_irq = 55, spi_rst = 21, spi_boot = 54; /* sysfs GPIO numbers */
static unsigned spi_speed = 500000;

static void spi_gpio_export(int gpio)
{
    char p[64], v[16];
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/value", gpio);
    if (access(p, F_OK) == 0) {
        return;
    }
    snprintf(v, sizeof(v), "%d", gpio);
    gpio_write("/sys/class/gpio/export", v);
}

static void spi_gpio_dir(int gpio, const char *dir)
{
    char p[64];
    spi_gpio_export(gpio);
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/direction", gpio);
    gpio_write(p, dir);
}

static void spi_gpio_set(int gpio, int v)
{
    char p[64];
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/value", gpio);
    gpio_write(p, v ? "1" : "0");
}

static int spi_gpio_get(int gpio)
{
    char p[64], b[4] = { 0 };
    snprintf(p, sizeof(p), "/sys/class/gpio/gpio%d/value", gpio);
    int fd = open(p, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    int n = read(fd, b, 1);
    close(fd);
    return n == 1 ? b[0] - '0' : -1;
}

static int spi_wait_irq(int level, int timeout_ms)
{
    long t0 = ms_now();
    while (ms_now() - t0 < timeout_ms) {
        if (spi_gpio_get(spi_irq) == level) {
            return 0;
        }
        usleep(1000);
    }
    return -1;
}

static int spi_xfer(const uint8_t *tx, uint8_t *rx, int len)
{
    struct spi_ioc_transfer t;
    memset(&t, 0, sizeof(t));
    t.tx_buf = (unsigned long)tx; /* always a TX buffer: an RX-only transfer corrupts bytes on the AP controller */
    t.rx_buf = (unsigned long)rx;
    t.len = len;
    t.speed_hz = spi_speed;
    t.bits_per_word = 8;
    return ioctl(spi_fd, SPI_IOC_MESSAGE(1), &t) == len ? 0 : -1;
}

/* One EZSP-SPI transaction: chip select, the command, then 0xFF until the reply starts and on to its 0xA7
 * terminator. Returns the reply length, 0 on a timeout, -1 on an error. */
static int spi_transaction(const uint8_t *cmd, int n, uint8_t *resp, int max, int timeout_ms)
{
    uint8_t ff = 0xff, rx = 0, junk[160];
    int len = 0, want = -1;
    spi_gpio_set(spi_cs, 0);
    usleep(20);
    if (spi_xfer(cmd, junk, n) < 0) {
        spi_gpio_set(spi_cs, 1);
        return -1;
    }
    long t0 = ms_now();
    for (;;) {
        if (spi_xfer(&ff, &rx, 1) < 0) {
            spi_gpio_set(spi_cs, 1);
            return -1;
        }
        if (rx != 0xff) {
            break;
        }
        if (ms_now() - t0 > timeout_ms) {
            spi_gpio_set(spi_cs, 1);
            return 0;
        }
    }
    resp[len++] = rx;
    if (rx == 0xfe || rx == 0xfd) {
        want = -2;
    }
    while (len < max) {
        if (spi_xfer(&ff, &rx, 1) < 0) {
            break;
        }
        resp[len++] = rx;
        if (want == -2) {
            want = rx + 3;
        }
        if (rx == 0xa7 && (want < 0 || len >= want)) {
            break;
        }
        if (want > 0 && len >= want) {
            break;
        }
    }
    spi_gpio_set(spi_cs, 1);
    usleep(2000);
    return len;
}

static int spi_bl_query(uint8_t *resp, int max)
{
    static const uint8_t q[] = { 0xfd, 0x01, 0x51, 0xa7 }; /* 'Q' */
    return spi_transaction(q, sizeof(q), resp, max, 3000);
}

/* The bootloader answers a query frame at once (QUERYFOUND, or the result code of the last block) and raises its
 * nHOST_INT when the result of that is ready to be fetched with another 'Q'. */
static int spi_bl_result(uint8_t *resp, int max)
{
    if (spi_wait_irq(0, 5000) != 0) {
        fprintf(stderr, "spi: no nHOST_INT within 5 s\n");
        return -1;
    }
    return spi_bl_query(resp, max);
}

/* Is the radio at its EZSP-SPI bootloader? Reads a pending reset indication first, then expects QUERYFOUND and the
 * query response (the bootloader version is in it). */
static int spi_bl_present(void)
{
    uint8_t resp[160];
    for (int attempt = 0; attempt < 4; attempt++) {
        int n = spi_bl_query(resp, sizeof(resp));
        if (n >= 3 && resp[0] == 0xfd && resp[2] == 0x1a) {
            n = spi_bl_result(resp, sizeof(resp));
            if (n >= 27 && resp[2] == 0x52) {
                fprintf(stderr, "spi: Gecko bootloader %d.%d (EZSP-SPI)\n", resp[26], resp[27]);
                return 1;
            }
            return 0;
        }
        if (n >= 2 && resp[0] == 0x00) {
            fprintf(stderr, "spi: reset indication, cause 0x%02x\n", resp[1]);
            usleep(300000);
            continue;
        }
        return 0;
    }
    return 0;
}

static void spi_enter_bootloader(void)
{
    fprintf(stderr, "spi: boot-select gpio%d low, reset pulse on gpio%d\n", spi_boot, spi_rst);
    spi_gpio_dir(spi_boot, "low");
    usleep(10000);
    spi_gpio_dir(spi_rst, "high");
    spi_gpio_set(spi_rst, 0);
    usleep(100000);
    spi_gpio_set(spi_rst, 1);
    if (spi_wait_irq(0, 2000) != 0) {
        fprintf(stderr, "spi: nHOST_INT not asserted after the reset\n");
    }
    usleep(50000);
    spi_gpio_dir(spi_boot, "high"); /* every later reset boots the application */
    usleep(1500000);                /* the bootloader starts talking after about a second */
}

static int spi_upload(const unsigned char *img, size_t size)
{
    uint8_t resp[160];
    size_t blocks = (size + 127) / 128;
    long t0 = ms_now();
    for (size_t i = 0; i < blocks; i++) {
        uint8_t frame[136];
        uint8_t blk = (uint8_t)((i + 1) & 0xff);
        size_t left = size - i * 128;
        frame[0] = 0xfd;
        frame[1] = 133;
        frame[2] = SOH;
        frame[3] = blk;
        frame[4] = (uint8_t)(255 - blk);
        memset(frame + 5, 0xff, 128);
        memcpy(frame + 5, img + i * 128, left < 128 ? left : 128);
        uint16_t crc = crc16(frame + 5, 128);
        frame[133] = (uint8_t)(crc >> 8);
        frame[134] = (uint8_t)(crc & 0xff);
        frame[135] = 0xa7;
        int ok = 0;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            int n = spi_transaction(frame, sizeof(frame), resp, sizeof(resp), 3000);
            if (n < 3 || resp[0] != 0xfd) {
                fprintf(stderr, "block %zu: no reply (attempt %d)\n", i + 1, attempt + 1);
                continue;
            }
            if (resp[2] == 0x18) {
                fprintf(stderr, "block %zu: the bootloader rejected the image (FILEABORT)\n", i + 1);
                return -1;
            }
            if (resp[2] != 0x19) {
                fprintf(stderr, "block %zu: reply 0x%02x (attempt %d)\n", i + 1, resp[2], attempt + 1);
                if (spi_wait_irq(0, 2000) == 0) {
                    spi_bl_query(resp, sizeof(resp));
                }
                continue;
            }
            n = spi_bl_result(resp, sizeof(resp));
            if (n >= 4 && resp[0] == 0xfd && resp[2] == ACK && resp[3] == blk) {
                ok = 1;
            } else {
                fprintf(stderr, "block %zu: no ACK (attempt %d)\n", i + 1, attempt + 1);
            }
        }
        if (!ok) {
            fprintf(stderr, "block %zu failed three times\n", i + 1);
            return -1;
        }
        if ((i + 1) % 200 == 0 || i + 1 == blocks) {
            fprintf(stderr, "%zu/%zu blocks (%ld s)\n", i + 1, blocks, (ms_now() - t0) / 1000);
        }
    }
    static const uint8_t eot[] = { 0xfd, 0x01, EOT, 0xa7 };
    int n = spi_transaction(eot, sizeof(eot), resp, sizeof(resp), 3000);
    if (n < 3 || resp[2] != 0x17) {
        fprintf(stderr, "EOT not acknowledged with FILEDONE\n");
        return -1;
    }
    spi_bl_result(resp, sizeof(resp));
    fprintf(stderr, "%zu bytes uploaded in %ld s; the bootloader reboots into the image\n", size, (ms_now() - t0) / 1000);
    return 0;
}

static int spi_flash(const char *dev, const char *lines, const char *enter, const unsigned char *buf, size_t size)
{
    if (lines && sscanf(lines, "%d,%d,%d,%d", &spi_cs, &spi_irq, &spi_rst, &spi_boot) != 4) {
        fprintf(stderr, "lines must be cs,irq,rst,boot (sysfs GPIO numbers)\n");
        return 2;
    }
    spi_fd = open(dev, O_RDWR);
    if (spi_fd < 0) {
        perror(dev);
        return 1;
    }
    uint8_t mode = SPI_MODE_0, bits = 8;
    ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed);
    spi_gpio_dir(spi_irq, "in");
    spi_gpio_dir(spi_cs, "high");
    fprintf(stderr, "spi %s (cs gpio%d, irq gpio%d, rst gpio%d, boot-select gpio%d)\n", dev, spi_cs, spi_irq, spi_rst, spi_boot);
    if (spi_bl_present()) {
        fprintf(stderr, "already at the bootloader\n");
    } else if (strcmp(enter, "none") == 0) {
        fprintf(stderr, "no bootloader on the SPI\n");
        return 1;
    } else {
        spi_enter_bootloader();
        if (!spi_bl_present()) {
            fprintf(stderr, "no bootloader after the reset\n");
            return 1;
        }
    }
    int r = spi_upload(buf, size);
    spi_gpio_dir(spi_boot, "high");
    close(spi_fd);
    return r == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *enter = "pins";
    int arg = 1;
    if (argc > 2 && strcmp(argv[1], "--enter") == 0) {
        enter = argv[2];
        arg = 3;
    }
    if (argc <= arg || (strcmp(enter, "pins") && strcmp(enter, "ezsp") && strcmp(enter, "spinel") && strcmp(enter, "none") && strcmp(enter, "spi"))) {
        fprintf(stderr, "usage: %s [--enter pins|ezsp|spinel|none] <file.gbl> [uart]\n"
                        "       %s --enter spi|none <file.gbl> /dev/spidevB.C [cs,irq,rst,boot]\n", argv[0], argv[0]);
        return 2;
    }
    const char *image = argv[arg];
    const char *uart = argc > arg + 1 ? argv[arg + 1] : "/dev/ttyMSM1";
    int pins = strcmp(enter, "pins") == 0;
    int spi = strcmp(enter, "spi") == 0 || strncmp(uart, "/dev/spidev", 11) == 0;
    if (spi && strcmp(enter, "spi") && strcmp(enter, "none")) {
        fprintf(stderr, "a radio on SPI takes --enter spi or none\n");
        return 2;
    }
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

    if (spi) {
        return spi_flash(uart, argc > arg + 2 ? argv[arg + 2] : NULL, enter, buf, st.st_size);
    }
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
        } else if (strcmp(enter, "spinel") == 0) {
            if (enter_bootloader_spinel(fd) != 0) {
                fprintf(stderr, "could not write to the RCP\n");
                return 1;
            }
        } else if (enter_bootloader_ezsp(fd) != 0) {
            fprintf(stderr, "could not ask the NCP for its bootloader\n");
            return 1;
        }
        if (wait_prompt(fd, pins ? 6000 : 10000) != 0) {
            /* The bootloader may use RTS/CTS differently from the application: try the other way once. Nothing has
             * been written to the radio yet; the upload starts only at the prompt. */
            set_flow_control(fd, !flow_control);
            if (wait_prompt(fd, 4000) != 0) {
                fprintf(stderr, "\nno bootloader prompt\n");
                return 1;
            }
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
