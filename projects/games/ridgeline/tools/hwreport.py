#!/usr/bin/env python3
"""What a console bench was doing when it stopped (make hwreport).

Reads the bench's rl_wd block (src/autotest.c) out of the console's memory
with dc-tool-ip once dcload-ip answers again, after a run that went quiet.
Leave the console on (no -o) until this has run.

Usage: tools/hwreport.py ridgeline-hwbench.elf
"""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

WHERE = {0: "starting", 1: "waiting for the display", 2: "frame begun", 3: "game update", 4: "drawing opaque",
         5: "drawing punch-through", 6: "drawing translucent", 7: "finishing the scene", 8: "scene sent",
         10: "after drawing (bench bookkeeping)"}


def nm_symbol(elf, name):
    out = subprocess.run(["sh-elf-nm", "-S", elf], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[3] in (name, "_" + name):
            return int(p[0], 16), int(p[1], 16)
    sys.exit(f"{elf} has no {name}: build it with make hwbench")


def addr2line(elf, addrs):
    if not addrs:
        return {}
    out = subprocess.run(["sh-elf-addr2line", "-f", "-s", "-e", elf] + [f"{a:x}" for a in addrs],
                         capture_output=True, text=True).stdout.splitlines()
    return {a: f"{out[2 * i]} {out[2 * i + 1]}" for i, a in enumerate(addrs) if 2 * i + 1 < len(out)}


def main():
    elf = sys.argv[1] if len(sys.argv) > 1 else "ridgeline-hwbench.elf"
    addr, size = nm_symbol(elf, "rl_wd")
    cache = pathlib.Path(os.environ.get("XDG_CACHE_HOME", pathlib.Path.home() / ".cache")) / "dc-hw"
    ip = os.environ.get("DC_IP") or (cache / "ip").read_text().strip()
    tool = os.environ.get("DC_TOOL") or "dc-tool-ip"
    if subprocess.run(["sh", "-c", f"command -v {tool}"], capture_output=True).returncode:
        tool = str(pathlib.Path.home() / "Dropbox/Games/ROMs/DREAMCAST/dcload-ip/dc-tool-ip")
    with tempfile.TemporaryDirectory() as d:
        out = pathlib.Path(d) / "rl_wd.bin"
        try:
            subprocess.run([tool, "-t", ip, "-n", "-q", "-d", str(out), "-a", hex(addr), "-s", str(size)],
                           capture_output=True, timeout=20)
        except subprocess.TimeoutExpired:
            pass
        if not out.exists() or out.stat().st_size < 32:
            sys.exit("no answer from dcload-ip: the console is off, still running the bench, or stopped hard")
        b = out.read_bytes()
    magic, frames, prints, in_print, stalls, where, tour, tour_frames = struct.unpack("<IIIIIiii", b[:32])
    if magic != 0x44574C52:
        sys.exit(f"no bench data at {addr:#x} (magic {magic:#x}): is this the ELF that ran?")
    print(f"frames {frames}, prints {prints}, at {WHERE.get(where, where)}, tour view {tour} frame {tour_frames}")
    if in_print:
        print("STUCK IN A PRINT: dcload-ip is waiting for dc-tool-ip's reply with interrupts off,")
        print("so nothing else runs. The game itself was fine; a packet went missing.")
    report = b[32:].split(b"\0", 1)[0].decode("latin1")
    if stalls:
        print(f"\nwatchdog report ({stalls} stall{'s' if stalls > 1 else ''}):\n{report}")
        codes = []
        for line in report.splitlines():
            for word in line.replace("pc", " ").replace("pr", " ").split():
                if len(word) == 8 and word.startswith("8c0"):
                    codes.append(int(word, 16))
        for a, where_ in addr2line(elf, sorted(set(codes))).items():
            print(f"  {a:08x}  {where_}")
    elif not in_print:
        print("no stall seen by the watchdog (still running, or stopped with interrupts off)")


if __name__ == "__main__":
    main()
