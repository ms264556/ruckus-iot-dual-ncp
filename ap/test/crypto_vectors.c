/* Runs iotd's crypto (iotd/crypto.c) on hex input, one operation per line, for test/crypto_test.py:
 *   sha256 <msg>
 *   hmac <key> <msg>
 *   seal <key> <nonce> <ad> <plaintext>     -> ciphertext||tag
 *   open <key> <nonce> <ad> <ciphertext||tag> -> plaintext, or "FAIL"
 *   x25519 <scalar> <point or ->
 * Empty fields are written as "-".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"

static size_t unhex(const char *s, uint8_t *out)
{
    if (!strcmp(s, "-")) {
        return 0;
    }
    size_t n = strlen(s) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

static void hex(const uint8_t *b, size_t n)
{
    if (!n) {
        fputs("-", stdout);
    }
    for (size_t i = 0; i < n; i++) {
        printf("%02x", b[i]);
    }
    putchar('\n');
}

int main(void)
{
    static char line[1 << 20];
    static uint8_t a[1 << 18], b[1 << 18], c[1 << 18], d[1 << 18], out[1 << 18];
    while (fgets(line, sizeof(line), stdin)) {
        char *op = strtok(line, " \n");
        if (!op) {
            continue;
        }
        if (!strcmp(op, "sha256")) {
            size_t n = unhex(strtok(NULL, " \n"), a);
            sha256(a, n, out);
            hex(out, 32);
        } else if (!strcmp(op, "hmac")) {
            size_t kn = unhex(strtok(NULL, " \n"), a);
            size_t mn = unhex(strtok(NULL, " \n"), b);
            hmac_sha256(a, kn, b, mn / 2, b + mn / 2, mn - mn / 2, out); /* split input exercises both parts */
            hex(out, 32);
        } else if (!strcmp(op, "seal") || !strcmp(op, "open")) {
            unhex(strtok(NULL, " \n"), a);
            unhex(strtok(NULL, " \n"), b);
            size_t adn = unhex(strtok(NULL, " \n"), c);
            size_t n = unhex(strtok(NULL, " \n"), d);
            if (op[0] == 's') {
                aead_encrypt(a, b, c, adn, d, n, out);
                hex(out, n + 16);
            } else if (aead_decrypt(a, b, c, adn, d, n - 16, out)) {
                hex(out, n - 16);
            } else {
                puts("FAIL");
            }
        } else if (!strcmp(op, "x25519")) {
            unhex(strtok(NULL, " \n"), a);
            char *p = strtok(NULL, " \n");
            if (!strcmp(p, "-")) {
                x25519(out, a, NULL);
            } else {
                unhex(p, b);
                x25519(out, a, b);
            }
            hex(out, 32);
        }
        fflush(stdout);
    }
    return 0;
}
