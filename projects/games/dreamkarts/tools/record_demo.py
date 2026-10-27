"""Turn a DK_RECORD serial log into the lobby server's demo races.

    make dreamkarts-record.elf VARIANT=record EXTRA_CFLAGS=-DDK_RECORD
    DK_ELF=dreamkarts-record.elf DK_EXIT_ON_LOG='DKR DONE' ./run-flycast.sh --skip-build > captures/record/record.log
    uv run tools/record_demo.py captures/record/record.log

Add EXTRA_CFLAGS="-DDK_RECORD -DDK_RECORD_ONLY=<track id>" to record just one
track; only that track's recordings are replaced.

Each race becomes server/demo/races/<track>-<n>.dkr.gz: "DKR3", u8 track,
u8 laps, u8 cc, u8 karts, u32 frames, u8 lobby, u8 race id, u8[2] zero,
then for every frame (12 a second from the green light) each kart's 29 wire
bytes (KART offsets 19-47: flags to the mine; the spear target is a kart
index here). The lobby and race id are what the record build seeded the
emeralds from; the server replays the race under them so watchers see the
gems where the bots took them.
"""
import gzip
import pathlib
import struct
import sys

SLUGS = ["beach", "coral", "harbour", "temple", "skyway", "cyberstone", "spiralcanyon", "tsudoken", "applevalley"]
KART_BYTES = 29        # what the record build prints (src/net.h NET_KART_BYTES)
ITEM_BYTE = 14         # held item, barrier, spun out, spear thrown backwards
SEQ_BYTE = 21          # spear sequence (low nibble) and mine sequence (high)
OUT = pathlib.Path(__file__).resolve().parent.parent / "server" / "demo" / "races"


def item_stats(frames, karts):
    """Emeralds taken, spears thrown, mines dropped and spin-outs over a race, from the item bytes."""
    taken = spears = mines = spins = 0
    held, seq, mseq, spun = [0] * karts, [0] * karts, [0] * karts, [False] * karts
    for data in frames:
        for k in range(karts):
            b, q = data[k * KART_BYTES + ITEM_BYTE], data[k * KART_BYTES + SEQ_BYTE]
            item, s, m, sp = b & 7, q & 15, q >> 4, bool(b & 16)
            if item and not held[k]:
                taken += 1
            if s and s != seq[k]:
                spears += 1
            if m and m != mseq[k]:
                mines += 1
            if sp and not spun[k]:
                spins += 1
            held[k], seq[k], mseq[k], spun[k] = item, s, m, sp
    return taken, spears, mines, spins


def main(log_path):
    races, cur = [], None
    for raw in open(log_path, "rb"):
        line = raw.decode("latin-1").strip()
        i = line.find("DKR ")
        if i < 0:
            continue
        parts = line[i:].split()
        if parts[1] == "BEGIN":
            track, n, laps, cc, karts = map(int, parts[2:7])
            lobby, race_id = map(int, parts[7:9]) if len(parts) >= 9 else (0, 0)
            cur = {"track": track, "n": n, "laps": laps, "cc": cc, "karts": karts, "lobby": lobby,
                   "race_id": race_id, "frames": []}
        elif parts[1] == "F" and cur is not None and len(parts) == 4:
            try:
                data = bytes.fromhex(parts[3])
            except ValueError:
                data = b""
            if int(parts[2]) == len(cur["frames"]) and len(data) == cur["karts"] * KART_BYTES:
                cur["frames"].append(data)
            else:
                print(f"skipping a garbled frame in race {cur['track']}-{cur['n']}", file=sys.stderr)
                cur = None   # a gap would make bots jump: drop the race
        elif parts[1] == "END" and cur is not None:
            if int(parts[2]) == len(cur["frames"]):
                races.append(cur)
            cur = None
    OUT.mkdir(parents=True, exist_ok=True)
    # Replace the recordings of the tracks in this log (a DK_RECORD_ONLY run
    # records one track and leaves the others alone).
    for slug in {SLUGS[r["track"]] for r in races}:
        for old in OUT.glob(f"{slug}-*.dkr.gz"):
            old.unlink()
    for r in races:
        body = b"DKR3" + bytes([r["track"], r["laps"], r["cc"], r["karts"]]) + struct.pack("<I", len(r["frames"]))
        body += bytes([r["lobby"], r["race_id"], 0, 0])
        body += b"".join(r["frames"])
        path = OUT / f"{SLUGS[r['track']]}-{r['n'] + 1}.dkr.gz"
        with open(path, "wb") as f:
            with gzip.GzipFile(fileobj=f, mode="wb", mtime=0, compresslevel=9) as g:
                g.write(body)
        taken, spears, mines, spins = item_stats(r["frames"], r["karts"])
        print(f"{path.name}: {len(r['frames'])} frames ({len(r['frames']) / 12:.0f} s), {path.stat().st_size // 1024} KiB,"
              f" race id {r['race_id']}: {taken} emeralds taken, {spears} spears, {mines} mines, {spins} spin-outs")
    print(f"{len(races)} races")


if __name__ == "__main__":
    main(sys.argv[1])
