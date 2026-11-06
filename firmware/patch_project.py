#!/usr/bin/env python3
"""Apply the per-target changes to a generated GSDK project (after patch_lfrco.py).

    patch_project.py <generated project directory> <mg21|mg12> <app version> <dead-man minutes> [ncp|thread]

Both targets:
  app.c      firmware/ncp_app_hooks.c appended, with NCP_APP_VERSION and NCP_DEADMAN_MINUTES
             (and firmware/thread_app_hooks.c for thread: the Series 1 watchdog the bootloader leaves on)
  app_init() calls ncp_app_hooks_init() (in main.c for the dual NCP, in app.c for the OpenThread samples)
  autogen/linkerfile_base.ld (linkerfile.ld in the OpenThread samples)
                               the Gecko bootloader's reset-cause word (first word of RAM) reserved
                               ahead of .stack, so a busy session cannot leave a value there that the
                               bootloader takes as a "reboot into bootloader" request on the next reset

thread only (scripts/build-thread.sh):
  <project>.project.mak          OPENTHREAD_CONFIG_PLATFORM_BOOTLOADER_MODE_ENABLE 1: the RCP answers
                                 Spinel's RESET_BOOTLOADER and the CLI has "reset bootloader", both
                                 through bootloader_rebootAndInstall(), the way back to the Gecko
                                 bootloader on a radio without a hardware entry (the R730's EFR32MG12)
  platform-abstraction/efr32/uartdrv_uart.c   every byte the host sends counts as the host being seen,
                                 which stops the dead-man reboot (the dual NCP uses cpcd's version query)

mg12 only (the R730's module; the brd4162a board config it is generated from is a dev kit):
  sl_device_init_dcdc_config.h   DCDC init in bypass mode. Series 1 parts come out of reset in
                                 bypass, which is safe whether or not the module has the DCDC
                                 inductor; EMU_DCDCPowerOff() (ENABLE 0) is not, it would brown
                                 out a module whose DVDD is fed by the DCDC.
  sl_rail_util_pa_config.h       PA supply 3300 mV (the PA is fed from VBAT in bypass)
  sl_board_control_config.h      no VCOM enable pin (PA5 on the dev kit), and no SPI flash
                                 shutdown (it clocks USART1 on PC6-PC9 for the dev kit's MX25)
  sl_rail_util_pti_config.h      packet trace off (no PTI pins on the module)
  autogen/sl_event_handler.c     ncp_itm_off() instead of sl_debug_swo_init() (which drives PF2
                                 push-pull): the ITM is switched off, so debug prints are no-ops
"""

import pathlib
import re
import sys


def sub_define(path: pathlib.Path, name: str, value: str) -> None:
    src = path.read_text()
    src, count = re.subn(rf"(#define\s+{name}\s+)\S+", rf"\g<1>{value}", src)
    if count != 1:
        raise SystemExit(f"error: {name} found {count} times in {path}")
    path.write_text(src)
    print(f"{path.name}: {name} {value}")


def main() -> int:
    if len(sys.argv) not in (5, 6) or sys.argv[2] not in ("mg21", "mg12", "mg21spi") or sys.argv[5:] not in ([], ["ncp"], ["thread"]):
        print(__doc__, file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1])
    target, version, deadman = sys.argv[2], sys.argv[3], int(sys.argv[4])
    thread = sys.argv[5:] == ["thread"]
    if not re.fullmatch(r"[A-Za-z0-9._+-]{1,60}", version):
        raise SystemExit(f"error: unsuitable version string {version!r}")

    hooks = (pathlib.Path(__file__).parent / "ncp_app_hooks.c").read_text()
    app = root / "app.c"
    src = app.read_text()
    if thread:
        # The OpenThread samples' app_init() lives in app.c and sets up the reset jump; the hooks run after it.
        src, count = re.subn(r"void app_init\(void\)\n\{\n(    OT_SETUP_RESET_JUMP\(argv\);\n)\}",
                             r"void ncp_app_hooks_init(void);\nvoid thread_app_hooks_init(void);\n\n"
                             r"void app_init(void)\n{\n\1    ncp_app_hooks_init();\n    thread_app_hooks_init();\n}", src)
        if count != 1:
            raise SystemExit(f"error: OpenThread app_init() not found in {app}")
        hooks += "\n" + (pathlib.Path(__file__).parent / "thread_app_hooks.c").read_text()
    app.write_text(src + "\n"
                   + f'#define NCP_APP_VERSION "{version}"\n'
                   + f"#define NCP_DEADMAN_MINUTES {deadman}\n"
                   + hooks)
    print(f"app.c: hooks appended (version {version}, dead-man {deadman} min)")

    if not thread:
        main_c = root / "main.c"
        src = main_c.read_text()
        src, count = re.subn(r"void app_init\(void\)\s*\{\s*\}",
                             "void ncp_app_hooks_init(void);\n\nvoid app_init(void)\n{\n  ncp_app_hooks_init();\n}", src)
        if count != 1:
            raise SystemExit(f"error: empty app_init() not found in {main_c}")
        main_c.write_text(src)
    print(f"{'app.c' if thread else 'main.c'}: app_init() calls ncp_app_hooks_init()")

    if target == "mg21spi":
        # The CPC SPI secondary driver names its pins from the primary's side: TX is the host's MOSI (an input on the
        # radio), RX the host's MISO. The R650 wires the host's MOSI to PC1 and MISO to PC0 (the reverse of the
        # brd4180b EXP header), and its HOST_INT line to PA4 (the MGM210L module layout).
        cfg = root / "config" / "sl_cpc_drv_secondary_spi_usart_exp_config.h"
        sub_define(cfg, "SL_CPC_DRV_SPI_EXP_TX_PIN", "1")
        sub_define(cfg, "SL_CPC_DRV_SPI_EXP_RX_PIN", "0")
        sub_define(cfg, "SL_CPC_DRV_SPI_EXP_RX_IRQ_PORT", "gpioPortA")
        sub_define(cfg, "SL_CPC_DRV_SPI_EXP_RX_IRQ_PIN", "4")
        print(f"{cfg.name}: TX PC1 (host MOSI), RX PC0 (host MISO), IRQ PA4 for the R650")

    if thread:
        mak = next(root.glob("*.project.mak"))
        src = mak.read_text()
        src, count = re.subn(r"^(C_DEFS \+= \\\n)", r"\1 '-DOPENTHREAD_CONFIG_PLATFORM_BOOTLOADER_MODE_ENABLE=1' \\\n", src,
                             count=1, flags=re.M)
        if count != 1:
            raise SystemExit(f"error: C_DEFS not found in {mak}")
        mak.write_text(src)
        print(f"{mak.name}: OPENTHREAD_CONFIG_PLATFORM_BOOTLOADER_MODE_ENABLE 1")

        uart = next(root.glob("gecko_sdk_*/protocol/openthread/platform-abstraction/efr32/uartdrv_uart.c"), None)
        if uart is None:
            print("no Spinel UART driver (CPC transport): cpcd's version query marks the host as seen")
    if thread and uart is not None:
        src = uart.read_text()
        src, count = re.subn(r"^(\s+)otPlatUartReceived\(", r"\1ncp_host_seen_mark();\n\1otPlatUartReceived(", src, flags=re.M)
        if count != 2:
            raise SystemExit(f"error: expected 2 otPlatUartReceived() calls in {uart}, found {count}")
        src = src.replace("static void processReceive(void);", "static void processReceive(void);\nvoid ncp_host_seen_mark(void);", 1)
        uart.write_text(src)
        print("uartdrv_uart.c: host bytes mark the host as seen")

    # The Gecko bootloader keeps its reset cause in the first word of RAM (btl_interface.c: SRAM_BASE; the bootloader's
    # own link reserves it as .bootloader_reset_section, an application's does not). The generated script puts .stack
    # there, so the deepest word of the main stack is that reset cause, and reset_classifyReset() reads a leftover
    # 0x00000001 as the Bluetooth stack's "reboot into DFU" request: after a busy session, the next software reset (cpcd's
    # start-up reset) parks the radio in its bootloader (seen on the R730, whose radio has no hardware bootloader entry).
    # Reserve the word ahead of the stack, as the bootloader's own link does.
    ld = root / "autogen" / ("linkerfile.ld" if thread else "linkerfile_base.ld")
    src = ld.read_text()
    reserve = ("  . = ORIGIN(RAM);\n\n"
               "  /* The Gecko bootloader's reset cause (BootloaderResetCause_t at SRAM_BASE); never part of the stack. */\n"
               "  .bootloader_reset_section (NOLOAD):\n  {\n    __ResetReasonStart__ = .;\n    KEEP(*(.bootloader_reset_section))\n"
               "    . += 4;\n  } > RAM\n")
    src, count = re.subn(r"^  \. = ORIGIN\(RAM\);\n", lambda _m: reserve, src, count=1, flags=re.M)
    if count != 1 or not re.search(r"\.bootloader_reset_section[\s\S]*?\n  \.stack ", src):
        raise SystemExit(f"error: could not reserve the bootloader reset cause ahead of .stack in {ld}")
    ld.write_text(src)
    print(f"{ld.name}: bootloader reset cause reserved at the start of RAM, ahead of .stack")

    if target == "mg12":
        config = root / "config"
        sub_define(config / "sl_device_init_dcdc_config.h", "SL_DEVICE_INIT_DCDC_ENABLE", "1")
        sub_define(config / "sl_device_init_dcdc_config.h", "SL_DEVICE_INIT_DCDC_BYPASS", "1")
        sub_define(config / "sl_rail_util_pa_config.h", "SL_RAIL_UTIL_PA_VOLTAGE_MV", "3300")
        sub_define(config / "sl_board_control_config.h", "SL_BOARD_ENABLE_VCOM", "0")
        sub_define(config / "sl_board_control_config.h", "SL_BOARD_DISABLE_MEMORY_SPI", "0")
        sub_define(config / "sl_rail_util_pti_config.h", "SL_RAIL_UTIL_PTI_MODE", "RAIL_PTI_MODE_DISABLED")
        events = root / "autogen" / "sl_event_handler.c"
        src = events.read_text()
        src, count = re.subn(r"^(\s*)sl_debug_swo_init\(\);",
                             r"\1ncp_itm_off(); /* instead of sl_debug_swo_init(): no SWO pin on the AP module */", src, flags=re.M)
        if count != 1:
            raise SystemExit(f"error: sl_debug_swo_init() found {count} times in {events}")
        src = src.replace('#include "sl_debug_swo.h"', '#include "sl_debug_swo.h"\nvoid ncp_itm_off(void);', 1)
        events.write_text(src)
        print("sl_event_handler.c: ncp_itm_off() instead of sl_debug_swo_init()")
    return 0


if __name__ == "__main__":
    sys.exit(main())
