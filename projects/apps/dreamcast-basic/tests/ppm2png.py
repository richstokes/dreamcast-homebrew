#!/usr/bin/env python3
"""Convert the host harness's binary PPM screen dumps to PNG (stdlib only)."""
import struct
import sys
import zlib


def convert(source, dest):
    data = open(source, 'rb').read()
    fields, at = [], 0
    while len(fields) < 4:
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end])
        at = end + 1
    assert fields[0] == b'P6' and fields[3] == b'255', fields
    width, height = int(fields[1]), int(fields[2])
    rows = b''.join(b'\0' + data[at + y * width * 3:at + (y + 1) * width * 3]
                    for y in range(height))

    def chunk(kind, body):
        return (struct.pack('>I', len(body)) + kind + body +
                struct.pack('>I', zlib.crc32(kind + body)))

    with open(dest, 'wb') as out:
        out.write(b'\x89PNG\r\n\x1a\n' +
                  chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2,
                                             0, 0, 0)) +
                  chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    convert(sys.argv[1], sys.argv[2])
