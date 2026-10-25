#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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
}

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
            bad = parse_port(val, &c->ble_port);
        } else if (!strcmp(key, "zigbee_port")) {
            bad = parse_port(val, &c->zigbee_port);
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
