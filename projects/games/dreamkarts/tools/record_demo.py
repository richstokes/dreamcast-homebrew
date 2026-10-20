"""Turn a DK_RECORD serial log into the lobby server's demo races.

    make dreamkarts-record.elf VARIANT=record EXTRA_CFLAGS=-DDK_RECORD
    DK_ELF=dreamkarts-record.elf DK_EXIT_ON_LOG='DKR DONE' ./run-flycast.sh --skip-build > captures/record/record.log
    uv run tools/record_demo.py captures/record/record.log

Add EXTRA_CFLAGS="-DDK_RECORD -DDK_RECORD_ONLY=<track id>" to record just one
track; only that track's recordings are replaced.

Each race becomes server/demo/races/<track>-<n>.dkr.gz: "DKR1", u8 track,
u8 laps, u8 cc, u8 karts, u32 frames, then for every frame (12 a second
from the green light) each kart's 21 wire bytes (KART offsets 19-39).
"""
import gzip
import pathlib
import struct
import sys

SLUGS = ["beach", "coral", "harbour", "temple", "skyway", "cyberstone"]
KART_BYTES = 21        # the demo files keep the build 10 body: the bots carry no items
WIRE_BYTES = 25        # what the record build prints (src/net.h NET_KART_BYTES)
OUT = pathlib.Path(__file__).resolve().parent.parent / "server" / "demo" / "races"


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
            cur = {"track": track, "n": n, "laps": laps, "cc": cc, "karts": karts, "frames": []}
        elif parts[1] == "F" and cur is not None and len(parts) == 4:
            try:
                data = bytes.fromhex(parts[3])
            except ValueError:
                data = b""
            if len(data) == cur["karts"] * WIRE_BYTES:
                data = b"".join(data[i * WIRE_BYTES:i * WIRE_BYTES + KART_BYTES] for i in range(cur["karts"]))
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
        body = b"DKR1" + bytes([r["track"], r["laps"], r["cc"], r["karts"]]) + struct.pack("<I", len(r["frames"]))
        body += b"".join(r["frames"])
        path = OUT / f"{SLUGS[r['track']]}-{r['n'] + 1}.dkr.gz"
        with open(path, "wb") as f:
            with gzip.GzipFile(fileobj=f, mode="wb", mtime=0, compresslevel=9) as g:
                g.write(body)
        print(f"{path.name}: {len(r['frames'])} frames ({len(r['frames']) / 12:.0f} s), {path.stat().st_size // 1024} KiB")
    print(f"{len(races)} races")


if __name__ == "__main__":
    main(sys.argv[1])
