#!/usr/bin/env python3
"""
Patches the Arduino WiFi framework to increase TX/RX buffer pool sizes.

The Arduino WiFi library hardcodes buffer counts in WiFiGeneric.cpp, ignoring
sdkconfig. Under sustained OINK/Spectrum deauth bursts the default 32-entry
dynamic TX pool is exhausted, returning ESP_ERR_NO_MEM (257) on every
esp_wifi_80211_tx() call.

This script is idempotent. It searches for the original block and replaces
it with bumped values. If the patch is already applied, it does nothing.

The framework file lives outside the project (in .platformio/packages/),
so this script runs as part of the PIO pre_build hook. It must be
re-run if the framework package is upgraded.

Re-apply after `platformio upgrade` or `pio pkg update`.
"""
import sys
import os
from pathlib import Path

OLD_BLOCK = "\t    cfg.static_tx_buf_num = 0;\n            cfg.dynamic_tx_buf_num = 32;\n\t    cfg.tx_buf_type = 1;\n            cfg.cache_tx_buf_num = 4;  // can't be zero!\n\t    cfg.static_rx_buf_num = 4;\n            cfg.dynamic_rx_buf_num = 32;"

NEW_BLOCK = "\t    // PORKCHOP PATCH (2026-06-16): bump dynamic TX 32→128.\n            // 64 was insufficient — ERR 257 after ~30 OKs suggests pool is reset\n            // to 32 somewhere (esp_wifi_set_config/channel?). Try 128 to see if\n            // the OK count scales linearly. RX is sufficient; bumping it added\n            // internal RAM that broke the display sprite.\n            // See scripts/patch_wifi_buffers.py. Re-apply after `platformio upgrade`.\n\t    cfg.static_tx_buf_num = 0;\n            cfg.dynamic_tx_buf_num = 128;\n\t    cfg.tx_buf_type = 1;\n            cfg.cache_tx_buf_num = 4;  // can't be zero!\n\t    cfg.static_rx_buf_num = 4;\n            cfg.dynamic_rx_buf_num = 32;"

MARKER = "PORKCHOP PATCH"

# Search paths for the framework WiFiGeneric.cpp
CANDIDATES = [
    Path.home() / ".platformio/packages/framework-arduinoespressif32/libraries/WiFi/src/WiFiGeneric.cpp",
    Path.home() / ".platformio/packages/framework-arduinoespressif32@3.20017.241212/libraries/WiFi/src/WiFiGeneric.cpp",
    Path.home() / ".platformio/packages/framework-arduinoespressif32@3.20016.0/libraries/WiFi/src/WiFiGeneric.cpp",
    Path.home() / ".platformio/packages/framework-arduinoespressif32@3.20009.0/libraries/WiFi/src/WiFiGeneric.cpp",
    Path.home() / ".platformio/packages/framework-arduinoespressif32@3.20006.221224/libraries/WiFi/src/WiFiGeneric.cpp",
]

def find_target() -> Path | None:
    for p in CANDIDATES:
        if p.exists():
            return p
    return None

def main() -> int:
    target = find_target()
    if target is None:
        print("[patch_wifi_buffers] WiFiGeneric.cpp not found in known framework paths", file=sys.stderr)
        return 0  # non-fatal: a future framework version may have moved it

    text = target.read_text(encoding="utf-8")
    if MARKER in text:
        # Patch is currently applied. By default we leave it (idempotent on rebuild).
        # Pass PORKCHOP_UNPATCH_WIFI=1 to remove the patch and restore Arduino defaults.
        if os.environ.get("PORKCHOP_UNPATCH_WIFI") == "1":
            unpatched = text.replace(NEW_BLOCK, OLD_BLOCK, 1)
            target.write_text(unpatched, encoding="utf-8")
            print(f"[patch_wifi_buffers] UNPATCHED: {target}")
            return 0
        # Marker present but NEW_BLOCK may be stale (different value).
        # Check if the actual value matches NEW_BLOCK. If not, re-apply.
        if NEW_BLOCK in text:
            print(f"[patch_wifi_buffers] already patched (current value): {target}")
            return 0
        # Stale marker with wrong value — re-apply by replacing marker+stale with NEW_BLOCK.
        # Find the line containing the marker and replace from there until the cfg line.
        import re
        pattern = re.compile(
            r"\t    // PORKCHOP PATCH[^\n]*\n[^\n]*dynamic_tx_buf_num[^\n]*\n[^\n]*\n[^\n]*\n[^\n]*\n[^\n]*",
            re.MULTILINE,
        )
        new_text, n = pattern.subn(NEW_BLOCK, text, count=1)
        if n:
            target.write_text(new_text, encoding="utf-8")
            print(f"[patch_wifi_buffers] re-applied (stale marker updated): {target}")
        else:
            print(f"[patch_wifi_buffers] WARNING: stale marker but couldn't re-apply", file=sys.stderr)
        return 0

    if OLD_BLOCK not in text:
        print(f"[patch_wifi_buffers] WARNING: original block not found in {target}", file=sys.stderr)
        print("[patch_wifi_buffers] framework version may have changed; manual update needed", file=sys.stderr)
        return 0  # non-fatal: don't break the build over this

    new_text = text.replace(OLD_BLOCK, NEW_BLOCK, 1)
    target.write_text(new_text, encoding="utf-8")
    print(f"[patch_wifi_buffers] patched: {target}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
