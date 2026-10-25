#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "iotd.h"

static bool verbose;

void log_set_verbose(bool v) { verbose = v; }
bool log_verbose(void) { return verbose; }

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static const char *level_name[] = { "error", "warn", "info", "debug" };

void log_msg(enum log_level lvl, const char *fmt, ...)
{
    if (lvl == LVL_DEBUG && !verbose) {
        return;
    }
    char line[1024];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    size_t off = strftime(line, sizeof(line), "%b %d %H:%M:%S", &tm);
    off += (size_t)snprintf(line + off, sizeof(line) - off, " iotd[%d]: %s: ", (int)getpid(), level_name[lvl]);
    va_list ap;
    va_start(ap, fmt);
    off += (size_t)vsnprintf(line + off, sizeof(line) - off, fmt, ap);
    va_end(ap);
    if (off > sizeof(line) - 2) {
        off = sizeof(line) - 2;
    }
    line[off++] = '\n';
    (void)!write(STDERR_FILENO, line, off);
}

void log_hex(enum log_level lvl, const char *tag, const uint8_t *b, size_t n)
{
    if (lvl == LVL_DEBUG && !verbose) {
        return;
    }
    char hex[3 * 256 + 8];
    size_t show = n > 256 ? 256 : n;
    size_t off = 0;
    for (size_t i = 0; i < show; i++) {
        off += (size_t)snprintf(hex + off, sizeof(hex) - off, "%02X ", b[i]);
    }
    if (show < n) {
        snprintf(hex + off, sizeof(hex) - off, "...");
    }
    log_msg(lvl, "%s (%zu): %s", tag, n, hex);
}
