"""ICONDATA and bounded, read-only extraction of standard VMU/Nexus images.

Layouts: https://vmu.falcogirgis.net/formats.html and filesystem.html;
directory/root compatibility checked against KOS dc/vmufs.h.
"""
import base64
import re
import struct

from vmu_validation import MAX_SAVE, validate_vms, game_label, first_icon_png, has_vms_icon

ICON_NAME = 'ICONDATA_VMS'
UNLOCK = bytes.fromhex('da69d0dac74ef836189279682db53086')
MAX_IMAGE = 128 * 1024
FILENAME = re.compile(r'[A-Za-z0-9_.! -]{1,12}')


def is_login(filename, data):
    return (filename.upper() == 'DCVMU_AUTH' or b'DCVMU-AUTH-V1' in data
            or any(data[i+48:i+64].rstrip(b'\0 ') == b'DCVMU_AUTH'
                   for i in range(0, len(data), 512)))


def validate_icondata(data):
    if not data or len(data) > MAX_SAVE or len(data) % 512:
        raise ValueError('Custom icons must contain complete VMU blocks.')
    mono, color = struct.unpack_from('<II', data, 16)
    spans = []
    for offset, size in ((mono, 128), (color, 544)):
        if offset:
            if offset < 24 or offset + size > len(data):
                raise ValueError('Custom icon artwork is truncated or has an invalid offset.')
            spans.append((offset, offset + size))
    if not spans or (len(spans) == 2 and max(s[0] for s in spans) < min(s[1] for s in spans)):
        raise ValueError('Custom icon artwork is missing or overlaps.')
    return mono, color


def build_icondata(label, mono, pixels, palette, unlock=False):
    """Accept exactly 32x32 bits/nibbles and sixteen ARGB4444 colors."""
    if (not re.fullmatch(r'[ -~]{1,16}', label)
            or not re.fullmatch(r'[01]{1024}', mono)
            or not re.fullmatch(r'[0-9a-fA-F]{1024}', pixels)
            or not re.fullmatch(r'[0-9a-fA-F]{64}', palette)):
        raise ValueError('Use a 1–16 character ASCII label and complete 32×32 artwork.')
    data = bytearray(1024)
    data[:16] = label.encode('ascii').ljust(16, b' ')
    struct.pack_into('<II', data, 16, 24, 152)
    data[24:152] = bytes(int(mono[i:i+8], 2) for i in range(0, 1024, 8))
    struct.pack_into('<16H', data, 152, *(int(palette[i:i+4], 16) for i in range(0, 64, 4)))
    data[184:696] = bytes.fromhex(pixels)
    if unlock:
        data[704:720] = UNLOCK
    return bytes(data)


def icon_header(data, monochrome=False):
    """Adapt artwork to the existing 640-byte preview decoder, never save bytes."""
    mono, color = validate_icondata(data)
    header = bytearray(640)
    struct.pack_into('<H', header, 64, 1)
    if color and not monochrome:
        header[96:640] = data[color:color+544]
    elif mono:
        struct.pack_into('<2H', header, 96, 0xFFFF, 0xF000)
        bits = ''.join(f'{b:08b}' for b in data[mono:mono+128])
        header[128:] = bytes.fromhex(bits)
    else:
        return None
    return bytes(header)


def icon_png(data, monochrome=False):
    header = icon_header(data, monochrome)
    return first_icon_png(header) if header else None


def swap_words(data):
    if len(data) % 4:
        raise ValueError('Nexus data must contain complete four-byte words.')
    result = bytearray(len(data))
    for i in range(4):
        result[i::4] = data[3-i::4]
    return bytes(result)


def _entry(entry, data=None, reason=''):
    rawname = entry[4:16].rstrip(b'\0 ')
    filename = rawname.decode('ascii', errors='replace')
    blocks, offset = struct.unpack_from('<HH', entry, 24)
    item = dict(filename=filename, blocks=blocks, header_offset=offset,
                name=filename, kind='data', reason=reason)
    try:
        if reason:
            raise ValueError(reason)
        if is_login(filename, data):
            raise ValueError('Login credentials — excluded')
        if entry[0] != 0x33:
            raise ValueError('VMU mini-game — not supported yet' if entry[0] == 0xCC else 'Unsupported file type')
        if not FILENAME.fullmatch(filename) or filename in ('.', '..'):
            raise ValueError('Filename is not supported by the Dreamcast client')
        if filename.upper() == ICON_NAME:
            if filename != ICON_NAME:
                raise ValueError('Custom icon filename must be ICONDATA_VMS')
            validate_icondata(data)
            item.update(kind='icon', header_offset=0, name='Imported VMU icons', game='VMU customisation')
        else:
            validate_vms(data, expected_offset=offset)
            item['game'] = game_label(filename, data, offset)
            item['name'] = item['game'][:64] or filename
        item['data'] = base64.b64encode(data).decode('ascii')
        item['has_icon'] = item['kind'] == 'icon' or has_vms_icon(data[offset*512:offset*512+640])
    except ValueError as error:
        item['reason'] = str(error)
    return item


def extract_image(data, filename):
    """Return preview rows; rejected entries never retain their payload bytes.

    Only standard 128 KiB volumes are accepted. Corrupt file chains are skipped;
    ambiguous/cross-linked files are both rejected, including links to auth files.
    """
    extension = filename.lower().rsplit('.', 1)[-1]
    if extension == 'dci':
        if not 544 <= len(data) <= MAX_SAVE + 32 or (len(data) - 32) % 512:
            raise ValueError('Invalid DCI size. Upload one complete Nexus file.')
        entry = data[:32]
        if struct.unpack_from('<H', entry, 24)[0] * 512 != len(data) - 32:
            raise ValueError('DCI directory size does not match its payload.')
        return [_entry(entry, swap_words(data[32:]))]
    if extension not in ('bin', 'vmu', 'dcm'):
        raise ValueError('Choose a .bin, .vmu, .dcm card image or a .dci file.')
    if len(data) != MAX_IMAGE:
        raise ValueError('Card images must be exactly 128 KiB (one standard VMU bank).')
    if extension == 'dcm':
        data = swap_words(data)
    root = data[255*512:]
    if root[:16] != b'\x55' * 16:
        raise ValueError('The card has no valid VMU root block.')
    fat_start, fat_size, dir_end, dir_size = struct.unpack_from('<4H', root, 0x46)
    if (fat_size != 1 or not 0 <= fat_start < 255 or not 1 <= dir_size <= 16
            or not dir_size <= dir_end < 255):
        raise ValueError('Unsupported or corrupt VMU filesystem layout.')
    directory = list(range(dir_end, dir_end-dir_size, -1))
    if fat_start in directory:
        raise ValueError('The card directory overlaps its allocation table.')
    system = {255, fat_start, *directory}
    user_blocks = struct.unpack_from('<H', root, 0x50)[0] or 200
    if not 1 <= user_blocks <= 255 or any(block < user_blocks for block in system):
        raise ValueError('Invalid VMU user region.')
    fat = struct.unpack_from('<256H', data, fat_start*512)
    entries, owners = [], {}
    for block in directory:
        for pos in range(block*512, (block+1)*512, 32):
            entry = data[pos:pos+32]
            if entry[0] == 0:
                continue
            start = struct.unpack_from('<H', entry, 2)[0]
            count = struct.unpack_from('<H', entry, 24)[0]
            chain, visited, reason = [], set(), ''
            if not 1 <= count <= 241:
                reason = 'Invalid file size'
            else:
                current = start
                for _ in range(count):
                    if current >= user_blocks or current in system or current in visited:
                        reason = 'Broken or looping block chain'
                        break
                    visited.add(current)
                    chain.append(current)
                    owners.setdefault(current, set()).add(len(entries))
                    current = fat[current]
                if not reason and current != 0xFFFA:
                    reason = 'Block chain does not end at the recorded file size'
            entries.append((entry, chain, reason))
    conflicts = set().union(*(ids for ids in owners.values() if len(ids) > 1)) if owners else set()
    rows = []
    for index, (entry, chain, reason) in enumerate(entries):
        if index in conflicts:
            reason = 'Blocks are shared with another file'
        raw = b''.join(data[block*512:(block+1)*512] for block in chain) if not reason else None
        rows.append(_entry(entry, raw, reason))
    return rows
