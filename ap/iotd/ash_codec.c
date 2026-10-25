#include <string.h>

#include "ash_codec.h"

uint16_t ash_crc16(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void ash_randomize(uint8_t *d, size_t n)
{
    uint8_t seed = 0x42;
    for (size_t i = 0; i < n; i++) {
        d[i] ^= seed;
        seed = (seed & 1) ? (uint8_t)((seed >> 1) ^ 0xB8) : (uint8_t)(seed >> 1);
    }
}

static bool needs_escape(uint8_t b)
{
    return b == ASH_FLAG || b == ASH_ESC || b == ASH_XON || b == ASH_XOFF || b == ASH_SUB || b == ASH_CAN;
}

static size_t put(uint8_t *out, size_t cap, size_t off, uint8_t b)
{
    if (needs_escape(b)) {
        if (off + 2 > cap) {
            return 0;
        }
        out[off++] = ASH_ESC;
        out[off++] = b ^ ASH_FLIP;
    } else {
        if (off + 1 > cap) {
            return 0;
        }
        out[off++] = b;
    }
    return off;
}

size_t ash_encode(const uint8_t *frame, size_t n, uint8_t *out, size_t cap)
{
    if (n == 0 || n > ASH_MAX_FRAME - 2) {
        return 0;
    }
    uint16_t crc = ash_crc16(frame, n);
    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        if (!(off = put(out, cap, off, frame[i]))) {
            return 0;
        }
    }
    if (!(off = put(out, cap, off, (uint8_t)(crc >> 8)))) {
        return 0;
    }
    if (!(off = put(out, cap, off, (uint8_t)crc))) {
        return 0;
    }
    if (off + 1 > cap) {
        return 0;
    }
    out[off++] = ASH_FLAG;
    return off;
}

void ash_decoder_reset(struct ash_decoder *d)
{
    d->len = 0;
    d->esc = false;
    d->bad = false;
}

int ash_decode_byte(struct ash_decoder *d, uint8_t b)
{
    switch (b) {
    case ASH_FLAG: {
        if (d->len == 0 && !d->bad) {
            d->esc = false;
            return 0; /* flag between frames */
        }
        int r;
        if (d->bad || d->len < 3) {
            r = -1;
        } else {
            uint16_t got = (uint16_t)((d->buf[d->len - 2] << 8) | d->buf[d->len - 1]);
            r = (ash_crc16(d->buf, d->len - 2) == got) ? (int)(d->len - 2) : -1;
        }
        ash_decoder_reset(d);
        return r;
    }
    case ASH_ESC:
        d->esc = true;
        return 0;
    case ASH_CAN:
        ash_decoder_reset(d);
        return -2;
    case ASH_SUB:
        d->bad = true;
        d->esc = false;
        return 0;
    case ASH_XON:
    case ASH_XOFF:
        return 0;
    default:
        if (d->esc) {
            b ^= ASH_FLIP;
            d->esc = false;
        }
        if (d->len < sizeof(d->buf)) {
            d->buf[d->len++] = b;
        } else {
            d->bad = true;
        }
        return 0;
    }
}
