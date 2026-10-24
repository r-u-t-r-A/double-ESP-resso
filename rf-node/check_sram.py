#!/usr/bin/env python3
"""Post-build check for the RF node ELF.

The PHY dump engine owns SRAM 0x40820000-0x4083FFFF while it captures
(main/iq_dump.c reserves it from the heap). Static code or data placed there
would be overwritten, so fail the build if any symbol lands in it. Also fail
if the RF test library symbols the capture path needs were not linked
(CONFIG_ESP_PHY_ENABLE_CERT_TEST).

usage: check_sram.py <nm> <elf>
"""
import subprocess
import sys

BANK_START = 0x40820000
BANK_END = 0x40840000
REQUIRED = ("adctrig", "force_rx_gain", "phy_set_rf_freq_offset", "phy_set_chanfreq")


def main():
    nm, elf = sys.argv[1], sys.argv[2]
    out = subprocess.run([nm, "-S", elf], check=True, capture_output=True, text=True).stdout
    names = set()
    top = 0
    bad = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        names.add(parts[-1])
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        size = int(parts[1], 16) if len(parts) == 4 else 0
        if not 0x40800000 <= addr < 0x40860000:
            continue
        end = addr + size
        if addr < BANK_START:
            top = max(top, end)
        if addr < BANK_END and end > BANK_START:
            bad.append(f"  {parts[-1]} at 0x{addr:08x}+{size}")
    missing = [s for s in REQUIRED if s not in names]
    if missing:
        sys.exit("check_sram: missing " + ", ".join(missing) +
                 " (is CONFIG_ESP_PHY_ENABLE_CERT_TEST set?)")
    if bad:
        sys.exit("check_sram: static symbols inside the dump bank "
                 f"0x{BANK_START:08x}-0x{BANK_END:08x}:\n" + "\n".join(bad[:20]))
    print(f"check_sram: static SRAM ends at 0x{top:08x}, "
          f"{(BANK_START - top) // 1024} KiB below the dump bank")


if __name__ == "__main__":
    main()
