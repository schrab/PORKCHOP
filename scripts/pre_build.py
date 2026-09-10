# Porkchop pre-build script
# Ensures model files exist and generates version info

Import("env")
import os
import subprocess
import sys
from datetime import datetime

# SCons doesn't always define __file__ for SConscript-loaded scripts.
# PROJECT_SRC_DIR is set by PIO to the project root; pre_build.py lives in scripts/.
_HERE = os.path.join(env.get("PROJECT_SRC_DIR"), "scripts")
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

def get_git_commit():
    """Get short git commit hash, or 'unknown' if not in a git repo"""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except Exception:
        pass
    return "unknown"

def patch_wifi_buffers():
    """Bump Arduino WiFi framework's TX/RX buffer pools.

    The Arduino WiFi library hardcodes buffer counts in WiFiGeneric.cpp,
    ignoring sdkconfig. Under sustained OINK/Spectrum deauth bursts the
    default 32-entry dynamic TX pool is exhausted, returning ESP_ERR_NO_MEM
    (257) on every esp_wifi_80211_tx() call. Idempotent — re-runs are a no-op.
    """
    try:
        import patch_wifi_buffers
        rc = patch_wifi_buffers.main()
        if rc:
            print(f"[pre_build] patch_wifi_buffers returned {rc}", file=sys.stderr)
    except Exception as e:
        # Non-fatal: don't break the build over a framework patch
        print(f"[pre_build] patch_wifi_buffers failed: {e}", file=sys.stderr)

def patch_int_wdt():
    """Bump Interrupt Watchdog timeout from 300ms to 5000ms.

    The Arduino framework ships pre-built libesp_system.a with
    CONFIG_ESP_INT_WDT_TIMEOUT_MS=300 in sdkconfig.h. board_build.sdkconfig
    is IGNORED for pre-built libs. OINK autosave SD I/O + WiFi driver state
    changes can block interrupts >300ms, triggering TG1WDT_SYS_RST.
    Patches the framework sdkconfig.h directly. Idempotent.
    """
    from pathlib import Path
    base = Path.home() / ".platformio/packages"
    if not base.exists():
        print("[patch_int_wdt] WARNING: .platformio/packages not found", file=sys.stderr)
        return
    candidates = []
    for pkg_dir in sorted(base.glob("framework-arduinoespressif32*"), reverse=True):
        sdk_base = pkg_dir / "tools/sdk/esp32s3"
        for variant in sdk_base.glob("*/include/sdkconfig.h"):
            candidates.append(variant)
    for p in candidates:
        text = p.read_text(encoding="utf-8")
        old = "#define CONFIG_ESP_INT_WDT_TIMEOUT_MS 300"
        new = "#define CONFIG_ESP_INT_WDT_TIMEOUT_MS 5000 /* PORKCHOP: 300->5000 for OINK autosave */"
        if "5000" in text and "INT_WDT_TIMEOUT_MS" in text:
            print(f"[patch_int_wdt] already patched: {p}")
            return
        if old in text:
            p.write_text(text.replace(old, new, 1), encoding="utf-8")
            print(f"[patch_int_wdt] patched 300->5000: {p}")
        else:
            print(f"[patch_int_wdt] WARNING: original value not found in {p}", file=sys.stderr)

# Apply framework patches IMMEDIATELY at SCons environment setup, so the patched
# framework sources are compiled when the framework's own build runs (not after).
# 2026-06-16: minimal patch — only dynamic TX 32→64. Earlier broader patch
# (also bumping RX) added ~13KB internal RAM that broke the display sprite.
patch_wifi_buffers()
patch_int_wdt()

def pre_build_callback(source, target, env):
    """Generate build info header"""
    build_info = {
        "build_time": datetime.now().isoformat(),
        "version": env.GetProjectOption("custom_version", "0.1.1"),
        "commit": get_git_commit()
    }

    info_path = os.path.join(env.get("PROJECT_SRC_DIR"), "build_info.h")
    with open(info_path, "w") as f:
        f.write("// Auto-generated build info\n")
        f.write("#pragma once\n")
        f.write(f'#define BUILD_TIME "{build_info["build_time"]}"\n')
        f.write(f'#define BUILD_VERSION "{build_info["version"]}"\n')
        f.write(f'#define BUILD_COMMIT "{build_info["commit"]}"\n')

env.AddPreAction("buildprog", pre_build_callback)
