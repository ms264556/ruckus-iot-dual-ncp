/* Appended to the generated app.c of the OpenThread images, after ncp_app_hooks.c (firmware/patch_project.py thread).
 *
 * The watchdog on Series 1 radios (the R730's EFR32MG12): its bootloader starts the application with the watchdog
 * running, the Zigbee stack in the dual NCP image feeds it, and OpenThread does not, so the RCP was reset about 33 s
 * after every start (32768 cycles of the 1 kHz ULFRCO) and the reset left it at the bootloader prompt. At start-up the
 * watchdog is switched off, or fed every second when the bootloader has locked it on; its state goes to the log.
 */
#include <openthread/logging.h>

#if defined(_SILICON_LABS_32B_SERIES_1) && defined(WDOG0)
static sl_sleeptimer_timer_handle_t thread_wdog_timer;

static void thread_wdog_feed(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;
  if (!(WDOG0->SYNCBUSY & WDOG_SYNCBUSY_CMD)) {
    WDOG0->CMD = WDOG_CMD_CLEAR;
  }
}
#endif

void thread_app_hooks_init(void)
{
#if defined(_SILICON_LABS_32B_SERIES_1) && defined(WDOG0)
  uint32_t ctrl = WDOG0->CTRL;
  if ((ctrl & WDOG_CTRL_EN) && !(ctrl & WDOG_CTRL_LOCK)) {
    while (WDOG0->SYNCBUSY & WDOG_SYNCBUSY_CTRL) {
    }
    WDOG0->CTRL = ctrl & ~WDOG_CTRL_EN;
    otLogCritPlat("watchdog was on (CTRL %08lx): switched off", (unsigned long)ctrl);
  } else if (ctrl & WDOG_CTRL_EN) {
    thread_wdog_feed(NULL, NULL);
    sl_sleeptimer_start_periodic_timer_ms(&thread_wdog_timer, 1000, thread_wdog_feed, NULL, 0, 0);
    otLogCritPlat("watchdog is locked on (CTRL %08lx): fed every second", (unsigned long)ctrl);
  } else {
    otLogCritPlat("watchdog off (CTRL %08lx)", (unsigned long)ctrl);
  }
#endif
}
