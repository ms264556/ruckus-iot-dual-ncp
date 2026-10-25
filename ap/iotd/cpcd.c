/* cpcd supervision: start Silicon Labs' cpcd as a child, restart it when it exits.
 *
 * cpcd's own trace output goes to IOTD_CPCD_LOG (truncated when iotd starts,
 * appended on restarts), not to our stderr: at trace_level debug it would drown our log.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "iotd.h"

static char cpcd_path[256];
static char cpcd_conf[256];
static pid_t child = -1;
static uint64_t started_at;
static uint64_t next_start;
static int log_flags = O_WRONLY | O_CREAT | O_TRUNC;
static bool restart_requested;
static uint64_t kill_deadline;

void cpcd_configure(const struct config *c)
{
    snprintf(cpcd_path, sizeof(cpcd_path), "%s", c->cpcd_path);
    snprintf(cpcd_conf, sizeof(cpcd_conf), "%s", c->cpcd_conf);
    next_start = 0;
}

pid_t cpcd_pid(void) { return child; }

static void start(uint64_t now)
{
    pid_t p = fork();
    if (p < 0) {
        LOGE("fork for cpcd: %s", strerror(errno));
        next_start = now + 5000;
        return;
    }
    if (p == 0) {
        int fd = open(IOTD_CPCD_LOG, log_flags, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > 2) {
                close(fd);
            }
        }
        /* Restore default signal dispositions in the child. */
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execl(cpcd_path, cpcd_path, "-c", cpcd_conf, (char *)NULL);
        _exit(127);
    }
    child = p;
    started_at = now;
    log_flags = O_WRONLY | O_CREAT | O_APPEND; /* keep history across restarts */
    LOGI("cpcd started: pid %d, %s -c %s, trace in %s", (int)p, cpcd_path, cpcd_conf, IOTD_CPCD_LOG);
}

void cpcd_tick(uint64_t now)
{
    if (child > 0) {
        int status;
        pid_t r = waitpid(child, &status, WNOHANG);
        if (r == child) {
            uint64_t lived = now - started_at;
            enum log_level lvl = restart_requested ? LVL_INFO : LVL_WARN;
            if (WIFEXITED(status)) {
                log_msg(lvl, "cpcd pid %d exited with status %d after %llu ms", (int)child, WEXITSTATUS(status),
                        (unsigned long long)lived);
            } else if (WIFSIGNALED(status)) {
                log_msg(lvl, "cpcd pid %d killed by signal %d after %llu ms", (int)child, WTERMSIG(status),
                        (unsigned long long)lived);
            }
            child = -1;
            if (restart_requested) {
                restart_requested = false;
                next_start = now;
            } else {
                /* A cpcd that dies within 5 s of starting is probably failing to open the
                 * UART or its socket; back off so the log stays readable. */
                next_start = now + (lived < 5000 ? 5000 : 1000);
            }
            link_request_down("cpcd exited");
        } else if (r > 0 || (r == 0 && restart_requested && now >= kill_deadline)) {
            if (r == 0) {
                LOGW("cpcd pid %d ignored SIGTERM, sending SIGKILL", (int)child);
                kill(child, SIGKILL);
                kill_deadline = now + 2000;
            }
        } else if (r < 0 && errno != EINTR) {
            LOGE("waitpid(%d): %s", (int)child, strerror(errno));
            child = -1;
            next_start = now + 1000;
        }
    }
    if (child < 0 && now >= next_start) {
        start(now);
    }
}

void cpcd_restart(const char *why)
{
    LOGI("restarting cpcd to reset the NCP: %s", why);
    if (child > 0) {
        if (!restart_requested) {
            kill(child, SIGTERM);
            kill_deadline = now_ms() + 2000;
        }
        restart_requested = true;
    } else {
        next_start = 0;
    }
}

bool cpcd_restarting(void) { return restart_requested; }

void cpcd_stop(void)
{
    if (child <= 0) {
        return;
    }
    LOGI("stopping cpcd pid %d", (int)child);
    kill(child, SIGTERM);
    for (int i = 0; i < 30; i++) {
        int status;
        if (waitpid(child, &status, WNOHANG) == child) {
            child = -1;
            return;
        }
        usleep(100000);
    }
    LOGW("cpcd pid %d did not exit on SIGTERM, sending SIGKILL", (int)child);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    child = -1;
}
