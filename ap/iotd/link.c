/* libcpc connection management.
 *
 * cpcd re-runs its reset sequence when the secondary resets, and dies and gets
 * restarted by cpcd.c when it crashes. In both cases the library calls our reset
 * callback from its own thread (we only set a flag) and/or endpoint reads fail
 * with -ECONNRESET. The main loop then tears everything down and retries
 * cpc_init() once a second until cpcd answers again. Endpoints are owned by the
 * modules; they are told to drop their handles before cpc_deinit().
 */
#include <signal.h>
#include <string.h>

#include "iotd.h"

/* cpcd's start-up reset sequence takes about 200 ms; bellows allows 2.5 s between
 * its RST and our RSTACK, which includes a cpcd restart, so keep the retry short. */
#define RETRY_MS 250

static cpc_handle_t handle;
static bool up;
static volatile sig_atomic_t reset_flag;
static const char *down_reason;
static uint64_t next_try;
static unsigned generation;
static bool failure_logged;

static void reset_cb(void)
{
    reset_flag = 1;
}

bool link_up(void) { return up; }
cpc_handle_t link_handle(void) { return handle; }
unsigned link_generation(void) { return generation; }

void link_request_down(const char *why)
{
    if (up && !down_reason) {
        down_reason = why;
    }
}

static void go_down(uint64_t now, const char *why)
{
    log_msg(cpcd_restarting() ? LVL_INFO : LVL_WARN, "cpc link down: %s", why);
    ble_link_down();
    zb_link_down();
    int r = cpc_deinit(&handle);
    if (r != 0) {
        LOGW("cpc_deinit: %d", r);
    }
    up = false;
    reset_flag = 0;
    down_reason = NULL;
    failure_logged = false;
    next_try = now + RETRY_MS;
}

void link_tick(uint64_t now)
{
    if (up && (reset_flag || down_reason)) {
        go_down(now, down_reason ? down_reason : "secondary or cpcd reset");
        return;
    }
    if (!up && now >= next_try) {
        int r = cpc_init(&handle, IOTD_CPCD_INSTANCE, false, reset_cb);
        if (r == 0) {
            up = true;
            generation++;
            reset_flag = 0;
            down_reason = NULL;
            LOGI("connected to cpcd instance %s (link generation %u)", IOTD_CPCD_INSTANCE, generation);
        } else {
            if (!failure_logged) {
                LOGI("cpc_init(%s) failed: %d, retrying every %d ms", IOTD_CPCD_INSTANCE, r, RETRY_MS);
                failure_logged = true;
            }
            next_try = now + RETRY_MS;
        }
    }
}

void link_shutdown(void)
{
    if (up) {
        ble_link_down();
        zb_link_down();
        cpc_deinit(&handle);
        up = false;
    }
}
