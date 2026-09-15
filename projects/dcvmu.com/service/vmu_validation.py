"""Bounded validation of raw VMS data files; never execute or unpack payloads."""
import binascii
import struct
import zlib

MAX_SAVE = 241 * 512  # Supports KOS expanded VMUs; still excludes whole-card images.
EYECATCH_BYTES = (0, 72 * 56 * 2, 512 + 72 * 56, 32 + 72 * 56 // 2)


def has_vms_icon(header_and_frame):
    """Check for a complete first frame following a stored VMS header."""
    return (len(header_and_frame) >= 640
            and 1 <= struct.unpack_from('<H', header_and_frame, 64)[0] <= 3)


def first_icon_png(header_and_frame):
    """Encode the first 32x32 icon as PNG, or return None if absent/truncated.

    Input starts at the validated header (dc/vmu_pkg.h), not the file start.
    The 16 little-endian ARGB4444 colors precede packed, high-nibble-first
    pixels. Later animation frames and eyecatch artwork are never rendered.
    PNG uses only stdlib code and preserves all four alpha bits.
    """
    if not has_vms_icon(header_and_frame):
        return None
    palette = [bytes(((color >> 8 & 15) * 17, (color >> 4 & 15) * 17,
                      (color & 15) * 17, (color >> 12) * 17))
               for color in struct.unpack_from('<16H', header_and_frame, 96)]
    scanlines = bytearray()
    for y in range(32):
        scanlines.append(0)  # PNG filter: None.
        for packed in header_and_frame[128 + y * 16:144 + y * 16]:
            scanlines.extend(palette[packed >> 4])
            scanlines.extend(palette[packed & 15])

    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data
                + struct.pack('>I', binascii.crc32(kind + data)))

    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', 32, 32, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(scanlines)) + chunk(b'IEND', b''))


def validate_vms(data):
    """Raise ValueError for unsupported/corrupt files, retaining exact valid bytes.

    Headers can start at a block offset, as recorded by the VMU directory.
    Return the validated header offset in blocks for faithful VMU installation.
    CRC-16/CCITT and layout match KOS vmu_pkg_build/vmu_pkg_parse.
    """
    if not data or len(data) > MAX_SAVE or len(data) % 512:
        raise ValueError('Save must contain 1-241 complete VMU blocks (maximum 120.5 KiB).')
    for offset in range(0, len(data), 512):
        header = data[offset:offset + 128]
        if len(header) < 128:
            continue
        icons, speed, eye, crc, payload = struct.unpack_from('<HHHHI', header, 64)
        if icons > 3 or eye >= len(EYECATCH_BYTES) or payload == 0:
            continue
        size = 128 + icons * 512 + EYECATCH_BYTES[eye] + payload
        end = offset + size
        # Allow final block padding, but not appended full blocks or truncated data.
        if end > len(data) or len(data) - end >= 512:
            continue
        checksum = binascii.crc_hqx(data[offset:offset + 70], 0)
        checksum = binascii.crc_hqx(b'\0\0', checksum)
        checksum = binascii.crc_hqx(data[offset + 72:end], checksum)
        if checksum == crc:
            return offset // 512
    raise ValueError('Not a supported VMS game save, or its header/checksum is corrupt.')


def header_metadata(header):
    """Read only standard descriptive fields from an already validated VMS header.

    Layout: KOS dc/vmu_pkg.h vmu_hdr_t. Text is conventionally Shift-JIS;
    replacement characters tolerate nonstandard encodings without failing pages.
    """
    if len(header) < 128:
        return {}
    def text(start, end):
        decoded = header[start:end].split(b'\0', 1)[0].decode('shift_jis', errors='replace')
        return ''.join(c for c in decoded if c.isprintable()).strip()
    return {'description': text(16, 48), 'vmu_label': text(0, 16),
            'application_id': text(48, 64)}
