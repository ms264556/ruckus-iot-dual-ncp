/* Appended to the generated app.c by scripts/build-firmware.sh (firmware/patch_project.py).
 *
 * sl_cpc_secondary_app_version() answers the version query cpcd makes in its start-up
 * handshake (cpcd logs it), so the host can tell which image the radio runs. Being asked
 * also proves a host reached the radio over CPC.
 *
 * With NCP_DEADMAN_MINUTES > 0 (lab builds only), a radio that no host has reached that many
 * minutes after boot reboots into its Gecko bootloader, where the host can upload another
 * image. That covers an image whose UART or CPC link does not come up on a radio that has no
 * hardware bootloader entry (the R730's EFR32MG12).
 */
#include <stdbool.h>
#include <stdio.h>
#include "em_device.h"
#include "sl_sleeptimer.h"
#include "btl_interface.h"

#ifndef NCP_APP_VERSION
#define NCP_APP_VERSION "unknown"
#endif
#ifndef NCP_DEADMAN_MINUTES
#define NCP_DEADMAN_MINUTES 0
#endif

static volatile bool ncp_host_seen;

/* Why this boot happened, for the host: the Gecko bootloader's reset-cause word (first word of RAM, reserved by our
 * linker patch so nothing else writes it; the bootloader invalidates the signature on entry but leaves the reason) and
 * the reset-cause register, both read in app_init(). The version string carries them: cpcd shows it with
 * --print-secondary-versions-and-exit, e.g. "c664710-mg12 rst=d00d0201 rmu=00000004". */
static char ncp_version_with_reset[64] = NCP_APP_VERSION;

static void ncp_capture_reset_info(void)
{
  uint32_t word = *(volatile uint32_t *)SRAM_BASE;
#if defined(RMU_PRESENT)
  uint32_t cause = RMU->RSTCAUSE;
#elif defined(EMU_RSTCAUSE_POR)
  uint32_t cause = EMU->RSTCAUSE;
#else
  uint32_t cause = 0xFFFFFFFFu;
#endif
  snprintf(ncp_version_with_reset, sizeof(ncp_version_with_reset), "%s rst=%08lx rmu=%08lx", NCP_APP_VERSION,
           (unsigned long)word, (unsigned long)cause);
}

const char *sl_cpc_secondary_app_version(void)
{
  ncp_host_seen = true;
  return ncp_version_with_reset;
}

#if NCP_DEADMAN_MINUTES > 0
static sl_sleeptimer_timer_handle_t ncp_deadman_timer;

static void ncp_deadman_expired(sl_sleeptimer_timer_handle_t *handle, void *data)
{
  (void)handle;
  (void)data;
  if (!ncp_host_seen) {
    bootloader_rebootAndInstall();
  }
}
#endif

/* Called instead of sl_debug_swo_init() where the module has no SWO pin (mg12). The ITM's state lives in the debug
 * domain, which a system reset leaves alone, so an earlier image could have left it enabled with no trace clock; the
 * debug print path (sl_debug_swo_write_*) only writes while ITMENA is set, so clearing it makes printing a no-op
 * instead of a wait on a FIFO that never drains. */
void ncp_itm_off(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  ITM->LAR = 0xC5ACCE55UL;
  ITM->TCR = 0;
  ITM->TER = 0;
}

void ncp_app_hooks_init(void)
{
  ncp_capture_reset_info();
#if NCP_DEADMAN_MINUTES > 0
  sl_sleeptimer_start_timer_ms(&ncp_deadman_timer, NCP_DEADMAN_MINUTES * 60u * 1000u,
                               ncp_deadman_expired, NULL, 0, 0);
#endif
}
