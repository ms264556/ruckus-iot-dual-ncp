#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#include "iotd.h"

void config_defaults(struct config *c)
{
    memset(c, 0, sizeof(*c));
    c->ble_enabled = true;
    c->zigbee_enabled = true;
    c->ble_port = 6639;
    c->zigbee_port = 6638;
    snprintf(c->cpcd_path, sizeof(c->cpcd_path), "/writable/data/bin/cpcd");
    snprintf(c->cpcd_conf, sizeof(c->cpcd_conf), "/writable/data/bin/cpcd.conf");
    c->api_mdns = true;
}

/* Standard base64 of exactly out_len bytes; false for anything else. */
static bool parse_base64(const char *s, uint8_t *out, size_t out_len)
{
    size_t n = 0, bits = 0;
    uint32_t acc = 0;
    for (; *s && *s != '='; s++) {
        const char *set = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const char *p = strchr(set, *s);
        if (!p || !*s) {
            return false;
        }
        acc = (acc << 6) | (uint32_t)(p - set);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == out_len) {
                return false;
            }
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n == out_len;
}

static void copy_text(char *dst, size_t cap, const char *val) { snprintf(dst, cap, "%s", val); }

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) {
        s++;
    }
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = 0;
    }
    return s;
}

static int parse_bool(const char *v, bool *out)
{
    if (!strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1") || !strcasecmp(v, "on")) {
        *out = true;
        return 0;
    }
    if (!strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcmp(v, "0") || !strcasecmp(v, "off")) {
        *out = false;
        return 0;
    }
    return -1;
}

static int parse_port(const char *v, int *out)
{
    char *end;
    long p = strtol(v, &end, 10);
    if (*end || p < 1 || p > 65535) {
        return -1;
    }
    *out = (int)p;
    return 0;
}

int config_load(struct config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = 0;
        }
        char *s = trim(line);
        if (!*s) {
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) {
            LOGW("%s:%d: expected key = value", path, lineno);
            continue;
        }
        *eq = 0;
        char *key = trim(s);
        char *val = trim(eq + 1);
        int bad = 0;
        if (!strcmp(key, "ble")) {
            bad = parse_bool(val, &c->ble_enabled);
        } else if (!strcmp(key, "zigbee")) {
            bad = parse_bool(val, &c->zigbee_enabled);
        } else if (!strcmp(key, "ble_port")) {
            if (!strcmp(val, "0")) {
                c->ble_port = 0; /* no listener */
            } else {
                bad = parse_port(val, &c->ble_port);
            }
        } else if (!strcmp(key, "ble_connect")) {
            uint32_t addr;
            int port;
            bad = *val && !net_parse_ip_port(val, &addr, &port);
            if (!bad) {
                snprintf(c->ble_connect, sizeof(c->ble_connect), "%s", val);
            }
        } else if (!strcmp(key, "zigbee_port")) {
            if (!strcmp(val, "0")) {
                c->zigbee_port = 0; /* no listener */
            } else {
                bad = parse_port(val, &c->zigbee_port);
            }
        } else if (!strcmp(key, "zigbee_connect")) {
            uint32_t addr;
            int port;
            bad = *val && !net_parse_ip_port(val, &addr, &port);
            if (!bad) {
                snprintf(c->zigbee_connect, sizeof(c->zigbee_connect), "%s", val);
            }
        } else if (!strcmp(key, "zigbee_allow")) {
            struct allow a;
            bad = !allow_parse(&a, val);
            if (!bad) {
                c->zigbee_allow = a;
            }
        } else if (!strcmp(key, "api_port")) {
            if (!strcmp(val, "0")) {
                c->api_port = 0;
            } else {
                bad = parse_port(val, &c->api_port);
            }
        } else if (!strcmp(key, "api_key")) {
            c->api_key_set = parse_base64(val, c->api_key, sizeof(c->api_key));
            bad = !c->api_key_set;
        } else if (!strcmp(key, "api_name")) {
            copy_text(c->api_name, sizeof(c->api_name), val);
        } else if (!strcmp(key, "api_friendly_name")) {
            copy_text(c->api_friendly_name, sizeof(c->api_friendly_name), val);
        } else if (!strcmp(key, "api_mac")) {
            copy_text(c->api_mac, sizeof(c->api_mac), val);
        } else if (!strcmp(key, "api_model")) {
            copy_text(c->api_model, sizeof(c->api_model), val);
        } else if (!strcmp(key, "api_bluetooth_mac")) {
            copy_text(c->api_bluetooth_mac, sizeof(c->api_bluetooth_mac), val);
        } else if (!strcmp(key, "api_allow")) {
            struct allow a;
            bad = !allow_parse(&a, val);
            if (!bad) {
                c->api_allow = a;
            }
        } else if (!strcmp(key, "api_mdns")) {
            bad = parse_bool(val, &c->api_mdns);
        } else if (!strcmp(key, "cpcd")) {
            snprintf(c->cpcd_path, sizeof(c->cpcd_path), "%s", val);
        } else if (!strcmp(key, "cpcd_conf")) {
            snprintf(c->cpcd_conf, sizeof(c->cpcd_conf), "%s", val);
        } else {
            LOGW("%s:%d: unknown key '%s' ignored", path, lineno, key);
        }
        if (bad) {
            LOGW("%s:%d: bad value '%s' for %s ignored", path, lineno, val, key);
        }
    }
    fclose(f);
    return 0;
}
