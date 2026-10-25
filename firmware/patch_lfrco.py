#!/usr/bin/env python3
"""Switch a generated GSDK project from the LFXO crystal to the internal LFRCO oscillator.

The radio module in the AP has no 32.768 kHz crystal. With the generator's default clock
tree, sl_device_init_lfxo() waits for a crystal that never starts and the low-frequency
consumers (RTOS tick, sleeptimer, power manager) never run, so the image never comes up.

    patch_lfrco.py <generated project directory>

Edits two generated files in place:
  autogen/sl_device_init_clocks.c   every CMU_CLOCK_SELECT_SET(<clock>, LFXO) -> LFRCO
  autogen/sl_event_handler.c        the sl_device_init_lfxo() call is commented out
"""

import pathlib
import re
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    root = pathlib.Path(sys.argv[1])

    clocks = root / "autogen" / "sl_device_init_clocks.c"
    src = clocks.read_text()
    src, count = re.subn(r"(CMU_CLOCK_SELECT_SET\([A-Z0-9_]+, )LFXO\)", r"\1LFRCO)", src)
    if count == 0 and "LFRCO" not in src:
        print(f"error: no LFXO clock selects found in {clocks}", file=sys.stderr)
        return 1
    clocks.write_text(src)
    print(f"clock selects switched to LFRCO: {count}")

    handler = root / "autogen" / "sl_event_handler.c"
    text = handler.read_text()
    marker = "/* sl_device_init_lfxo(); no LFXO crystal on the AP module */"
    if marker not in text:
        if "  sl_device_init_lfxo();" not in text:
            print(f"error: sl_device_init_lfxo() call not found in {handler}", file=sys.stderr)
            return 1
        text = text.replace("  sl_device_init_lfxo();", "  " + marker)
        handler.write_text(text)
    print("sl_device_init_lfxo() skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
