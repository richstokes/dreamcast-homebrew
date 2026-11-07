"""Read-only GDI/ISO9660 reader and bounded Sega PRS decoder."""
from pathlib import Path
import shlex
import struct


def prs(data, limit=32 * 1024 * 1024):
    pos = bits = control = 0
    out = bytearray()

    def byte():
        nonlocal pos
        if pos >= len(data):
            raise ValueError("Truncated PRS input")
        v = data[pos]
        pos += 1
        return v

    def bit():
        nonlocal bits, control
        if not bits:
            control, bits = byte(), 8
        v = control & 1
        control >>= 1
        bits -= 1
        return v

    while len(out) < limit:
        if bit():
            out.append(byte())
            continue
        if bit():
            pair = byte() | (byte() << 8)
            if pair == 0:
                return bytes(out)
            offset = (pair >> 3) - 8192
            length = pair & 7
            length = length + 2 if length else byte() + 1
        else:
            length = (bit() << 1) | bit()
            length += 2
            offset = byte() - 256
        if len(out) + offset < 0:
            raise ValueError("Invalid PRS back-reference")
        for _ in range(length):
            out.append(out[len(out) + offset])
    raise ValueError("PRS output exceeds safety bound")


class Disc:
    def __init__(self, path):
        self.path = Path(path).expanduser().resolve()
        tracks = [shlex.split(x) for x in self.path.read_text().splitlines()[1:]]
        track = next(t for t in tracks if int(t[1]) >= 45000 and t[2] == "4")
        self.lba, self.sector = int(track[1]), int(track[3])
        self.offset = int(track[5])
        self.file = (self.path.parent / track[4]).open("rb")
        self.payload = 16 if self.sector == 2352 else 0
        pvd = self.read(self.lba + 16, 2048)
        if pvd[1:6] != b"CD001":
            raise ValueError("Expected Mode 1 ISO9660 high-density track")
        root = pvd[156:190]
        self.files = {}
        self._directory(struct.unpack_from("<I", root, 2)[0],
                        struct.unpack_from("<I", root, 10)[0], "")

    def read(self, lba, size):
        if lba < self.lba or size > 64 * 1024 * 1024:
            raise ValueError("Invalid ISO extent")
        out = bytearray()
        for sector in range((size + 2047) // 2048):
            self.file.seek(self.offset + (lba - self.lba + sector) * self.sector + self.payload)
            chunk = self.file.read(2048)
            if len(chunk) != 2048:
                raise ValueError("Truncated disc image")
            out.extend(chunk)
        return bytes(out[:size])

    def _directory(self, lba, size, prefix):
        data, pos = self.read(lba, size), 0
        while pos < len(data):
            n = data[pos]
            if not n:
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = data[pos:pos + n]
            pos += n
            name = rec[33:33 + rec[32]].decode("ascii").split(";")[0]
            if name in ("\x00", "\x01"):
                continue
            extent = struct.unpack_from("<I", rec, 2)[0]
            length = struct.unpack_from("<I", rec, 10)[0]
            if rec[25] & 2:
                self._directory(extent, length, prefix + name + "/")
            else:
                self.files[prefix + name] = (extent, length)

    def get(self, name):
        matches = [p for p in self.files if Path(p).name == name.upper()]
        if len(matches) != 1:
            raise ValueError(f"Expected exactly one {name}; found {matches}")
        return self.read(*self.files[matches[0]])
