"""Quick parse of ESP-IDF coredump ELF to find panic reason + registers.

The coredump is an ELF file with PT_NOTE segments. Each note contains
register state, task name, and panic reason. The relevant note types
for ESP-IDF coredump are:
  - 0x41514900 (NT_ESP32_INFO) — panic reason + reason string
  - 0x41514901 (NT_ESP32_REGISTERS) — register dump
  - 0x41514902 (NT_ESP32_MEMDUMP) — memory regions

This script reads the ELF, finds notes, and prints what it can.
"""
import struct
import sys
from pathlib import Path


# ELF constants
ELFCLASS32 = 1
ELFCLASS64 = 2
ELFDATA2LSB = 1
ELFDATA2MSB = 2
EI_CLASS = 4
EI_DATA = 5
PT_NOTE = 4
NT_ESP32_INFO = 0x41514900
NT_ESP32_REGISTERS = 0x41514901


def parse_elf_notes(data: bytes):
    if data[:4] != b"\x7fELF":
        # ESP coredump BIN format has a custom header before the ELF.
        # Header: 4 bytes coredump size LE, 4 bytes app SHA256 (first 4), 4 bytes version,
        #         4 bytes reserved, then ELF magic.
        # The 4 bytes at offset 4-7 are the first 4 of the app SHA256 — print so user
        # can verify against the firmware that was running.
        if len(data) >= 8:
            print(f"[parse] coredump app SHA256 prefix: {data[4:8].hex()}... (compare to firmware.elf SHA)")
        elf_off = data.find(b"\x7fELF")
        if elf_off > 0:
            print(f"[parse] detected coredump BIN format ({elf_off}-byte header), skipping")
            data = data[elf_off:]
        else:
            print(f"[parse] not an ELF file (magic: {data[:4].hex()})")
            return

    # Parse ELF header
    ei_class = data[4]
    ei_data = data[5]
    endian = "<" if ei_data == ELFDATA2LSB else ">"

    if ei_class == ELFCLASS32:
        # 32-bit ELF: e_phoff at 28, e_phentsize at 42, e_phnum at 44
        e_phoff = struct.unpack_from(endian + "I", data, 28)[0]
        e_phentsize = struct.unpack_from(endian + "H", data, 42)[0]
        e_phnum = struct.unpack_from(endian + "H", data, 44)[0]
    else:
        # 64-bit ELF
        e_phoff = struct.unpack_from(endian + "Q", data, 32)[0]
        e_phentsize = struct.unpack_from(endian + "H", data, 54)[0]
        e_phnum = struct.unpack_from(endian + "H", data, 56)[0]

    print(f"[parse] ELF class={ei_class} endian={endian} phoff={e_phoff} phentsize={e_phentsize} phnum={e_phnum}")

    # Parse program headers
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if ei_class == ELFCLASS32:
            p_type, p_offset, p_size = struct.unpack_from(endian + "III", data, off)[:3]
        else:
            p_type, p_offset, p_size = struct.unpack_from(endian + "QQQ", data, off)[:3]

        if p_type != PT_NOTE:
            continue

        print(f"[parse] PT_NOTE segment: offset={p_offset} size={p_size}")

        # Parse notes
        pos = p_offset
        end = p_offset + p_size
        while pos < end:
            if ei_class == ELFCLASS32:
                namesz, descsz, ntype = struct.unpack_from(endian + "III", data, pos)
                hdr = 12
            else:
                namesz, descsz, ntype = struct.unpack_from(endian + "QQQ", data, pos)
                hdr = 12
            pos += hdr

            # name is padded to 4 bytes
            name_end = pos + ((namesz + 3) & ~3)
            desc_end = pos + ((descsz + 3) & ~3)
            desc = data[pos:pos + descsz]
            pos = name_end

            if ntype == NT_ESP32_INFO:
                # struct: panic_reason (uint32), panic_reason_str (string), task_name (string)
                if len(desc) >= 4:
                    reason_code = struct.unpack_from("<I", desc, 0)[0]
                    print(f"[parse]   NT_ESP32_INFO: panic_reason=0x{reason_code:08x}")
                    # Reason string is at offset 4
                    rest = desc[4:]
                    if rest:
                        nul = rest.find(b"\x00")
                        if nul > 0:
                            print(f"[parse]   reason_str: {rest[:nul].decode('utf-8', errors='replace')}")
                        # Task name follows
                        if nul >= 0:
                            task = rest[nul + 1:]
                            tnul = task.find(b"\x00")
                            if tnul > 0:
                                print(f"[parse]   task_name: {task[:tnul].decode('utf-8', errors='replace')}")
            elif ntype == NT_ESP32_REGISTERS:
                # struct esp_xtensa_debug_reg_block for Xtensa, or esp_riscv_reg_block
                # Just dump raw bytes — caller can interpret
                print(f"[parse]   NT_ESP32_REGISTERS: {descsz} bytes")
                # Try to extract PC at offset 0 (Xtensa debug_regs[0] is PC)
                if len(desc) >= 4:
                    pc = struct.unpack_from("<I", desc, 0)[0]
                    print(f"[parse]     PC = 0x{pc:08x}")
                if len(desc) >= 16:
                    a0, a1 = struct.unpack_from("<II", desc, 4)
                    print(f"[parse]     A0 = 0x{a0:08x}  A1 = 0x{a1:08x}")
            else:
                print(f"[parse]   note type=0x{ntype:08x} size={descsz}")

            pos = desc_end


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <coredump.bin>")
        return 1
    path = Path(sys.argv[1])
    if not path.exists():
        print(f"[parse] file not found: {path}")
        return 1
    data = path.read_bytes()
    print(f"[parse] reading {len(data)} bytes from {path}")
    parse_elf_notes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
