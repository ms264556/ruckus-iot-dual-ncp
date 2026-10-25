/* ASH codec unit test. The expected bytes were produced by bellows' own ASH
 * implementation (bellows.ash), so passing here means we frame exactly like the
 * client Home Assistant uses. Build and run with `make test`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ash_codec.h"

static int failures;

static size_t unhex(const char *s, uint8_t *out)
{
    size_t n = 0;
    while (s[0] && s[1]) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void dump(const char *tag, const uint8_t *b, size_t n)
{
    printf("  %s:", tag);
    for (size_t i = 0; i < n; i++) {
        printf(" %02x", b[i]);
    }
    printf("\n");
}

/* frame_hex = control + data field as bellows builds it (data already randomized);
 * wire_hex = the stuffed frame with CRC, without the trailing 0x7E. */
static void check_encode(const char *name, const char *frame_hex, const char *wire_hex)
{
    uint8_t frame[300], wire[600], out[600];
    size_t fn = unhex(frame_hex, frame), wn = unhex(wire_hex, wire);
    wire[wn++] = ASH_FLAG;
    size_t on = ash_encode(frame, fn, out, sizeof(out));
    if (on != wn || memcmp(out, wire, wn)) {
        printf("FAIL encode %s\n", name);
        dump("want", wire, wn);
        dump("got ", out, on);
        failures++;
    } else {
        printf("ok   encode %s\n", name);
    }
    /* and decode it back */
    struct ash_decoder d;
    ash_decoder_reset(&d);
    int r = 0;
    for (size_t i = 0; i < wn; i++) {
        r = ash_decode_byte(&d, wire[i]);
        if (r && i + 1 != wn) {
            printf("FAIL decode %s: early result %d at byte %zu\n", name, r, i);
            failures++;
            return;
        }
    }
    if (r != (int)fn || memcmp(d.buf, frame, fn)) {
        printf("FAIL decode %s: result %d\n", name, r);
        dump("want", frame, fn);
        dump("got ", d.buf, r > 0 ? (size_t)r : 0);
        failures++;
    } else {
        printf("ok   decode %s\n", name);
    }
}

int main(void)
{
    /* bellows: RstFrame().to_bytes() == c038bc etc. */
    check_encode("RST", "c0", "c038bc");
    check_encode("RSTACK v2 software", "c1020b", "c1020b0a52");
    check_encode("ACK 1", "81", "816059");
    check_encode("NAK 1", "a1", "a1443b");
    check_encode("DATA(0,0,0) version v13", "004221a859", "004221a8597c05");
    check_encode("DATA(3,retx,5) 13-byte payload", "3d43a1a9722a14b05a904f23ad5d", "3d43a1a9722a14b05a904f23ad5d48ab");
    check_encode("ERROR ack timeouts", "c20251", "c20251a8bd");

    /* randomization: version command 00 00 00 0d -> 42 21 a8 59 */
    uint8_t v[4] = { 0x00, 0x00, 0x00, 0x0d };
    ash_randomize(v, 4);
    if (memcmp(v, "\x42\x21\xa8\x59", 4)) {
        printf("FAIL randomize\n");
        dump("got", v, 4);
        failures++;
    } else {
        printf("ok   randomize\n");
    }

    /* stuffing of every reserved byte (bellows _stuff_bytes) */
    uint8_t raw[8] = { 0x7e, 0x7d, 0x11, 0x13, 0x18, 0x1a, 0x00, 0xff };
    uint8_t want[64], got[64];
    size_t wn = unhex("7d5e7d5d7d317d337d387d3a00ff", want);
    uint16_t crc = ash_crc16(raw, 8);
    /* append the CRC bytes to the expectation the same way the encoder would */
    size_t gn = ash_encode(raw, 8, got, sizeof(got));
    size_t head = wn;
    if (gn < head || memcmp(got, want, head)) {
        printf("FAIL stuffing\n");
        dump("want", want, wn);
        dump("got ", got, gn);
        failures++;
    } else {
        printf("ok   stuffing (crc %04x)\n", crc);
    }

    /* decoder: CANCEL prefix as bellows sends before RST, then bad CRC, then good */
    struct ash_decoder d;
    ash_decoder_reset(&d);
    int r = 0, cancels = 0, bads = 0, frames = 0;
    uint8_t stream[64];
    size_t sn = unhex("1a1a1ac038bc7ec038bd7e7e7ec038bc7e", stream);
    for (size_t i = 0; i < sn; i++) {
        r = ash_decode_byte(&d, stream[i]);
        if (r == -2) {
            cancels++;
        } else if (r == -1) {
            bads++;
        } else if (r > 0) {
            frames++;
        }
    }
    if (cancels != 3 || bads != 1 || frames != 2) {
        printf("FAIL decoder stream: cancels %d bads %d frames %d\n", cancels, bads, frames);
        failures++;
    } else {
        printf("ok   decoder stream (cancel, bad crc, empty flags, good)\n");
    }

    printf(failures ? "%d FAILURE(S)\n" : "all passed (%d)\n", failures);
    return failures ? 1 : 0;
}
