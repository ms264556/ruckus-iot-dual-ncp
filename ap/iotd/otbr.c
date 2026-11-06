/* Thread border router on the AP itself (thread_border_router = yes): otbr-agent as a supervised child on the radio's
 * UART, with the AP's LAN bridge as its infrastructure link. While it runs, IPv6 forwarding is on and the bridge keeps
 * taking router advertisements (accept_ra 2, so the AP keeps its own address); the values found at start are put back
 * when iotd stops. otbr-agent's output goes to IOTD_OTBR_LOG; its settings (the Thread network) stay in
 * /writable/data/thread across restarts. Its mDNS responder (mdnsd, for the border agent's _meshcop._udp and the SRP
 * advertising proxy) is started before it and kept running, with the gateway firewall's mDNS accept rule (mdns.c). */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "iotd.h"

#define IOTD_OTBR_LOG "/tmp/otbr-agent.log"
/* The border router's own mDNS responder (scripts/build-otbr.sh builds it with this pid file and the socket
 * /var/run/otbr-mdnsd, so it never meets the firmware's mdnsd on /var/run/mdnsd). It daemonizes itself. */
#define IOTD_MDNSD_PID "/var/run/otbr-mdnsd.pid"
#define MDNSD_CHECK_MS 5000

static struct {
    char path[256];
    char mdnsd[256];
    uint64_t mdnsd_checked_at;
    bool cpc;                 /* the RCP is behind cpcd (thread_cpc) */
    int cpc_iid;
    char uart[64];
    char infra[16];
    pid_t child;
    uint64_t started_at, next_start;
    int log_flags;
    char forwarding[8], accept_ra[8]; /* the values before we changed them; empty: unread */
    char link_local[48];              /* the address we gave the infrastructure link ("fe80::.../64"); empty: none */
} O = { .child = -1 };

static int read_sysctl(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    out[n] = 0;
    out[strcspn(out, "\n")] = 0;
    return 0;
}

static void write_sysctl(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0 || write(fd, value, strlen(value)) != (ssize_t)strlen(value)) {
        LOGW("otbr: cannot set %s to %s: %s", path, value, strerror(errno));
    }
    if (fd >= 0) {
        close(fd);
    }
}

static void infra_path(char *buf, size_t cap, const char *leaf)
{
    snprintf(buf, cap, "/proc/sys/net/ipv6/conf/%s/%s", O.infra, leaf);
}

/* Whether the infrastructure link has an IPv6 link-local address (/proc/net/if_inet6: address, index, prefix length,
 * scope 20 = link, flags, name). */
static bool infra_has_link_local(void)
{
    FILE *f = fopen("/proc/net/if_inet6", "r");
    char addr[40], name[32];
    unsigned idx, plen, scope, flags;
    bool found = false;
    if (!f) {
        return false;
    }
    while (!found && fscanf(f, "%39s %x %x %x %x %31s", addr, &idx, &plen, &scope, &flags, name) == 6) {
        found = scope == 0x20 && strcmp(name, O.infra) == 0;
    }
    fclose(f);
    return found;
}

/* The border router sends router advertisements from the infrastructure link's link-local address and stops border
 * routing without one. Some firmware (3.6.2) strips it from the LAN bridge (no IPv6 there), so while the border router
 * runs the link gets the standard EUI-64 one from its MAC, and that address alone is removed afterwards. */
static void link_local_ensure(void)
{
    if (infra_has_link_local()) {
        return; /* the link's own, or the one we added, still there */
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", O.infra);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0 || ioctl(fd, SIOCGIFHWADDR, &ifr) != 0) {
        LOGW("otbr: %s has no IPv6 link-local address and its MAC cannot be read: %s", O.infra, strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return;
    }
    close(fd);
    const unsigned char *m = (const unsigned char *)ifr.ifr_hwaddr.sa_data;
    snprintf(O.link_local, sizeof(O.link_local), "fe80::%02x%02x:%02xff:fe%02x:%02x%02x/64", m[0] ^ 0x02, m[1], m[2], m[3], m[4], m[5]);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "ip -6 addr add %s dev %s 2>/dev/null", O.link_local, O.infra);
    if (system(cmd) == 0) {
        LOGI("otbr: %s had no IPv6 link-local address; added %s for the border router's router advertisements", O.infra, O.link_local);
    } else {
        LOGW("otbr: adding %s to %s failed", O.link_local, O.infra);
        O.link_local[0] = 0;
    }
}

static void link_local_remove(void)
{
    if (!O.link_local[0]) {
        return;
    }
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "ip -6 addr del %s dev %s 2>/dev/null", O.link_local, O.infra);
    if (system(cmd) != 0) {
        LOGW("otbr: removing %s from %s failed", O.link_local, O.infra);
    }
    O.link_local[0] = 0;
}

static void routing_on(void)
{
    char ra[96];
    infra_path(ra, sizeof(ra), "accept_ra");
    if (!O.forwarding[0] && read_sysctl("/proc/sys/net/ipv6/conf/all/forwarding", O.forwarding, sizeof(O.forwarding)) < 0) {
        O.forwarding[0] = 0;
    }
    if (!O.accept_ra[0] && read_sysctl(ra, O.accept_ra, sizeof(O.accept_ra)) < 0) {
        O.accept_ra[0] = 0;
    }
    /* accept_ra first: turning forwarding on would otherwise make the bridge drop its own address's router. */
    write_sysctl(ra, "2");
    write_sysctl("/proc/sys/net/ipv6/conf/all/forwarding", "1");
    link_local_ensure();
}

static void routing_restore(void)
{
    char ra[96];
    link_local_remove();
    infra_path(ra, sizeof(ra), "accept_ra");
    if (O.forwarding[0]) {
        write_sysctl("/proc/sys/net/ipv6/conf/all/forwarding", O.forwarding);
    }
    if (O.accept_ra[0]) {
        write_sysctl(ra, O.accept_ra);
    }
}

/* The pid in the responder's pid file when that process is alive, else 0. */
static pid_t mdnsd_pid(void)
{
    char buf[16];
    if (read_sysctl(IOTD_MDNSD_PID, buf, sizeof(buf)) < 0) {
        return 0;
    }
    pid_t pid = (pid_t)atoi(buf);
    return pid > 1 && kill(pid, 0) == 0 ? pid : 0;
}

/* Starts mdnsd unless it runs; it forks into the background, so the child it leaves behind is reaped here. True when
 * it was started. */
static bool mdnsd_ensure(void)
{
    if (!O.mdnsd[0] || mdnsd_pid()) {
        return false;
    }
    if (access(O.mdnsd, X_OK) != 0) {
        LOGW("otbr: %s: %s; the border router runs without mDNS (no border agent or SRP advertising)", O.mdnsd, strerror(errno));
        O.mdnsd[0] = 0;
        return false;
    }
    pid_t p = fork();
    if (p < 0) {
        LOGE("otbr: fork for mdnsd: %s", strerror(errno));
        return false;
    }
    if (p == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        /* Answer on the infrastructure link only (otbr/0002-mdnsresponder-interface-allow-list.patch), not on the
         * AP's WLAN, guest or internal bridges. */
        setenv("MDNSD_INTERFACES", O.infra, 1);
        execl(O.mdnsd, O.mdnsd, (char *)NULL);
        _exit(127);
    }
    waitpid(p, NULL, 0);
    usleep(500 * 1000);
    LOGI("otbr: mdnsd started: pid %d", (int)mdnsd_pid());
    return true;
}

static void mdnsd_stop(void)
{
    pid_t pid = mdnsd_pid();
    if (pid) {
        LOGI("otbr: stopping mdnsd pid %d", (int)pid);
        kill(pid, SIGTERM);
    }
}

static void start(uint64_t now)
{
    char url[160];
    mdnsd_ensure();
    if (O.cpc) {
        snprintf(url, sizeof(url), "spinel+cpc://%s?iid=%d&iid-list=0", IOTD_CPCD_INSTANCE, O.cpc_iid);
    } else {
        snprintf(url, sizeof(url), "spinel+hdlc+uart://%s?uart-baudrate=115200&uart-flow-control", O.uart);
    }
    routing_on();
    pid_t p = fork();
    if (p < 0) {
        LOGE("otbr: fork: %s", strerror(errno));
        O.next_start = now + 5000;
        return;
    }
    if (p == 0) {
        int fd = open(IOTD_OTBR_LOG, O.log_flags, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) {
                close(fd);
            }
        }
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execl(O.path, O.path, "-v", "-d", "5", "-I", "wpan0", "-B", O.infra, url, (char *)NULL);
        _exit(127);
    }
    O.child = p;
    O.started_at = now;
    O.log_flags = O_WRONLY | O_CREAT | O_APPEND; /* keep history across restarts */
    LOGI("otbr: otbr-agent started: pid %d, radio %s, infrastructure %s, log in %s", (int)p, O.uart, O.infra, IOTD_OTBR_LOG);
}

int otbr_init(const struct config *c)
{
    snprintf(O.path, sizeof(O.path), "%s", c->otbr_path);
    snprintf(O.mdnsd, sizeof(O.mdnsd), "%s", c->mdnsd_path);
    snprintf(O.uart, sizeof(O.uart), "%s", c->thread_uart);
    O.cpc = c->thread_cpc;
    O.cpc_iid = c->thread_cpc_iid;
    if (O.cpc) {
        snprintf(O.uart, sizeof(O.uart), "cpcd (%s)", IOTD_CPCD_INSTANCE);
    }
    snprintf(O.infra, sizeof(O.infra), "%s", c->thread_infra);
    O.log_flags = O_WRONLY | O_CREAT | O_TRUNC;
    O.next_start = 0;
    if (access(O.path, X_OK) != 0) {
        LOGE("otbr: %s: %s", O.path, strerror(errno));
        return -1;
    }
    LOGI("otbr: Thread border router on this AP: %s on %s, infrastructure link %s", O.path, O.uart, O.infra);
    return 0;
}

void otbr_tick(uint64_t now)
{
    if (O.child > 0) {
        int status;
        pid_t r = waitpid(O.child, &status, WNOHANG);
        if (r == O.child) {
            uint64_t lived = now - O.started_at;
            if (WIFEXITED(status)) {
                LOGW("otbr: otbr-agent pid %d exited with status %d after %llu ms", (int)O.child, WEXITSTATUS(status),
                     (unsigned long long)lived);
            } else if (WIFSIGNALED(status)) {
                LOGW("otbr: otbr-agent pid %d killed by signal %d after %llu ms", (int)O.child, WTERMSIG(status),
                     (unsigned long long)lived);
            }
            O.child = -1;
            /* One that dies at once (a radio that does not answer, a kernel without dummy interfaces) is retried slowly. */
            O.next_start = now + (lived < 10000 ? 30000 : 2000);
        } else if (r < 0 && errno != EINTR) {
            LOGE("otbr: waitpid(%d): %s", (int)O.child, strerror(errno));
            O.child = -1;
            O.next_start = now + 5000;
        }
    }
    if (O.child < 0 && now >= O.next_start) {
        start(now);
    }
    if (O.child > 0 && now - O.mdnsd_checked_at >= MDNSD_CHECK_MS) {
        /* mdnsd gone, and the gateway firewall's mDNS rule, which the AP's nat script drops whenever it rebuilds the
         * chain. otbr-agent reconnects at once when mdnsd goes, fails while there is none and never registers its
         * services again, so a restarted mdnsd means restarting otbr-agent too. */
        O.mdnsd_checked_at = now;
        link_local_ensure();
        if (mdnsd_ensure()) {
            LOGI("otbr: restarting otbr-agent pid %d so that it registers its services with the new mdnsd", (int)O.child);
            kill(O.child, SIGTERM);
        }
        if (O.mdnsd[0]) {
            mdns_firewall_check(now);
        }
    }
}

void otbr_stop(void)
{
    if (O.child > 0) {
        LOGI("otbr: stopping otbr-agent pid %d", (int)O.child);
        kill(O.child, SIGTERM);
        int i;
        for (i = 0; i < 50 && waitpid(O.child, NULL, WNOHANG) != O.child; i++) {
            usleep(100000);
        }
        if (i == 50) {
            LOGW("otbr: otbr-agent pid %d did not exit on SIGTERM, sending SIGKILL", (int)O.child);
            kill(O.child, SIGKILL);
            waitpid(O.child, NULL, 0);
        }
        O.child = -1;
    }
    mdnsd_stop();
    mdns_firewall_remove();
    routing_restore();
}
