/* ASH (UG101) framing: byte stuffing, CRC-16 CCITT, data-field randomization.
 * Pure functions, no I/O, so they can be unit-tested on the build host.
 */
#ifndef ASH_CODEC_H
#define ASH_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ASH_FLAG  0x7E
#define ASH_ESC   0x7D
#define ASH_XON   0x11
#define ASH_XOFF  0x13
#define ASH_SUB   0x18
#define ASH_CAN   0x1A
#define ASH_FLIP  0x20

#define ASH_CONTROL_ACK     0x80
#define ASH_CONTROL_NAK     0xA0
#define ASH_CONTROL_RST     0xC0
#define ASH_CONTROL_RSTACK  0xC1
#define ASH_CONTROL_ERROR   0xC2
#define ASH_VERSION         0x02

/* Reset / error codes carried by RSTACK and ERROR frames */
#define ASH_RESET_SOFTWARE          0x0B
#define ASH_ERROR_ACK_TIMEOUTS      0x51

/* Largest frame we accept or build: control + data field (EZSP over CPC is at most
 * 220 bytes; EZSP_MAX_FRAME_LENGTH is 202) plus the CRC. */
#define ASH_MAX_DATA     220
#define ASH_MAX_FRAME    (1 + ASH_MAX_DATA + 2)
#define ASH_MAX_ENCODED  (2 * ASH_MAX_FRAME + 1)

uint16_t ash_crc16(const uint8_t *d, size_t n);

/* XOR the data field with the LFSR sequence (seed 0x42, polynomial 0xB8).
 * The same call randomizes and de-randomizes. */
void ash_randomize(uint8_t *d, size_t n);

/* frame = control byte followed by the (already randomized) data field.
 * Writes the stuffed frame, CRC and closing flag to out. Returns the number of
 * bytes written, or 0 if cap is too small. */
size_t ash_encode(const uint8_t *frame, size_t n, uint8_t *out, size_t cap);

struct ash_decoder {
    uint8_t buf[ASH_MAX_FRAME + 1];
    size_t len;
    bool esc;
    bool bad;
};

void ash_decoder_reset(struct ash_decoder *d);

/* Feed one byte. Returns:
 *   > 0  a complete frame of that many bytes (control + data, CRC verified and
 *        stripped) is in d->buf; the decoder is ready for the next frame
 *   0    frame in progress or nothing to report
 *   -1   a frame ended with a bad CRC, bad length or a substitute byte
 *   -2   a cancel byte discarded the frame in progress */
int ash_decode_byte(struct ash_decoder *d, uint8_t b);

#endif
