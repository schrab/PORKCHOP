"""Dump and decode the ESP32 coredump partition.

Reads 64KB from offset 0x790000 (matches partitions_esp32s3_mini.csv),
saves to coredump.bin, then runs espcoredump.py to print the register
state and call stack.

Usage:
    python scripts/dump_coredump.py [PORT]

PORT defaults to COM35.
"""
import os
import sys
import subprocess
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
FW_ELF = PROJECT_ROOT / ".pio" / "build" / "esp32s3-mini" / "firmware.elf"
COREDUMP_BIN = PROJECT_ROOT / "coredump.bin"
COREDUMP_OFFSET = 0x790000
COREDUMP_SIZE = 0x10000
ESPTOOL = Path.home() / ".platformio/packages/tool-esptoolpy@1.40501.0/esptool.py"
ESPCOREDUMP = Path.home() / ".platformio/packages/framework-espidf/components/espcoredump/espcoredump.py"


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM35"
    chip = "esp32s3"

    # Find tools if hardcoded paths don't exist
    global ESPTOOL, ESPCOREDUMP
    if not ESPTOOL.exists():
        # Search via python -m esptool as fallback
        esptool_cmd = [sys.executable, "-m", "esptool"]
    else:
        esptool_cmd = [sys.executable, str(ESPTOOL)]
    if not ESPCOREDUMP.exists():
        print(f"[dump_coredump] WARNING: espcoredump.py not found at {ESPCOREDUMP}", file=sys.stderr)
        return 1

    if not FW_ELF.exists():
        print(f"[dump_coredump] firmware.elf not found at {FW_ELF}", file=sys.stderr)
        print("  Run `pio run -e esp32s3-mini` first", file=sys.stderr)
        return 1

    print(f"[dump_coredump] Reading coredump from {port} @ 0x{COREDUMP_OFFSET:08x} ({COREDUMP_SIZE} bytes)...")
    rc = subprocess.run(
        esptool_cmd + [
            "--chip", chip,
            "--port", port,
            "read_flash",
            f"0x{COREDUMP_OFFSET:x}",
            f"0x{COREDUMP_SIZE:x}",
            str(COREDUMP_BIN),
        ],
        check=False,
    )
    if rc.returncode != 0:
        print(f"[dump_coredump] esptool failed (rc={rc.returncode})", file=sys.stderr)
        return rc.returncode

    if not COREDUMP_BIN.exists() or COREDUMP_BIN.stat().st_size == 0:
        print("[dump_coredump] no coredump data read (partition empty?)", file=sys.stderr)
        return 1

    print(f"[dump_coredump] saved {COREDUMP_BIN.stat().st_size} bytes to {COREDUMP_BIN}")
    print(f"[dump_coredump] decoding with espcoredump.py...")
    return subprocess.run(
        [sys.executable, str(ESPCOREDUMP), "info_corefile",
         "--corefile-format", "raw",
         "--corefile", str(COREDUMP_BIN),
         "--elf", str(FW_ELF)],
        check=False,
    ).returncode


if __name__ == "__main__":
    sys.exit(main())
