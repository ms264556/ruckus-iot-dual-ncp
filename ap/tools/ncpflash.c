/* ncpflash: flash a GBL image onto the AP's built-in IoT radio through its Gecko bootloader.
 *
 *   ncpflash <file.gbl> [uart]        uart defaults to /dev/ttyMSM1
 *
 * Enters the bootloader with the radio's reset and boot-select lines (/proc/v54bsp/iot_rst
 * and iot_rcv), uploads over XMODEM-CRC at 115200 from the bootloader menu, then runs the
 * new application. Nothing else may hold the UART while it runs.
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
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.gbl> [uart]\n", argv[0]);
        return 2;
    }
    signal(SIGALRM, on_alarm);
    alarm(180);

    struct stat st;
    if (stat(argv[1], &st) != 0) {
        perror(argv[1]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    unsigned char *buf = malloc(st.st_size);
    if (!buf || fread(buf, 1, st.st_size, f) != (size_t)st.st_size) {
        fprintf(stderr, "read failed\n");
        return 2;
    }
    fclose(f);
    fprintf(stderr, "image %s: %ld bytes\n", argv[1], (long)st.st_size);

    const char *uart = argc > 2 ? argv[2] : "/dev/ttyMSM1";
    fprintf(stderr, "uart %s\n", uart);
    int fd = serial_open(uart, 115200);
    if (fd < 0) {
        return 1;
    }
    fprintf(stderr, "entering bootloader\n");
    enter_bootloader();

    if (read_until(fd, "BL >", 6000) != 0) {
        fprintf(stderr, "\nno bootloader prompt\n");
        return 1;
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
    release_run();
    fprintf(stderr, "done\n");
    return 0;
}
