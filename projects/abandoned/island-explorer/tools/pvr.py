"""Decode PVR files for Blender and normalize their payloads for PowerVR RAM."""
import struct
import numpy as np
from PIL import Image


def spread(v):
    r = 0
    for bit in range(10):
        r |= ((v >> bit) & 1) << (2 * bit)
    return r


def hardware_payload(raw, layout):
    # Standard PVRT layout 2 omits four bytes of the hardware mip-chain prefix.
    # KOS utils/pvrtex/file_pvr.c documents this file/VRAM difference. The 1x1
    # RGB565 texel lives at byte 2 in the file but byte 6 in VRAM; the 256x256
    # level starts at 43,692 in the file and 43,696 in VRAM. Uploading the file
    # verbatim shifts every twiddled level and creates a dotted/checker pattern.
    # VQ mip chains already have their correct prefix and must stay unchanged.
    return bytes(4) + raw if layout == 2 else raw


def decode(raw, pixel, layout, w, h):
    if layout not in (1, 2, 3, 4):
        raise ValueError(f'Unsupported PVR layout {layout}')
    yy, xx = np.indices((h, w))
    tw = np.array([spread(i) for i in range(max(w, h))])
    if layout in (3, 4):
        mip = sum(max(1, s * s // 4) for s in (2 ** i for i in range(w.bit_length() - 1))) if layout == 4 else 0
        blocks = np.frombuffer(raw, np.uint8, count=w * h // 4, offset=2048 + mip)
        palette = np.frombuffer(raw, '<u2', count=1024)
        idx = blocks[tw[xx // 2] * 2 + tw[yy // 2]] * np.uint16(4) + (xx % 2) * 2 + yy % 2
        values = palette[idx]
    else:
        mip = 2 + sum(2 * s * s for s in (2 ** i for i in range(w.bit_length() - 1))) if layout == 2 else 0
        values = np.frombuffer(raw, '<u2', count=w*h, offset=mip)[tw[xx] * 2 + tw[yy]]
    if pixel == 1:
        rgba = np.stack(((values >> 11) * 255 // 31, ((values >> 5) & 63) * 255 // 63, (values & 31) * 255 // 31, np.full_like(values, 255)), -1)
    elif pixel == 2:
        rgba = np.stack((((values >> 8) & 15) * 17, ((values >> 4) & 15) * 17, (values & 15) * 17, (values >> 12) * 17), -1)
    elif pixel == 0:
        rgba = np.stack((((values >> 10) & 31) * 255 // 31, ((values >> 5) & 31) * 255 // 31, (values & 31) * 255 // 31, (values >> 15) * 255), -1)
    else:
        raise ValueError(f'Unsupported PVR pixel format {pixel}')
    return Image.fromarray(rgba.astype('uint8'))


def archive(data, prefix, image_dir):
    assert data[:4] == b'PVMH'
    flags, count = struct.unpack_from('<HH', data, 8)
    stride = 2 + (28 if flags & 1 else 0) + (2 if flags & 2 else 0) + (2 if flags & 4 else 0) + (4 if flags & 8 else 0)
    p = struct.unpack_from('<I', data, 4)[0] + 8
    textures = []
    for i in range(count):
        name = data[12 + i * stride + 2:12 + i * stride + 30].split(b'\0')[0].decode('ascii')
        p = data.find(b'PVRT', p)
        if p < 0:
            raise ValueError('Missing PVR texture')
        size = struct.unpack_from('<I', data, p+4)[0]
        pixel, layout = data[p+8:p+10]
        w, h = struct.unpack_from('<HH', data, p+12)
        raw = data[p+16:p+8+size]
        # Decode the file layout before adding the hardware's missing prefix.
        n = 2048 + w*h//4 if layout in (3,4) else w*h*2
        if layout == 4:
            n += sum(max(1, (2**j)**2//4) for j in range(w.bit_length()-1))
        elif layout == 2:
            n += 2 + sum(2*(2**j)**2 for j in range(w.bit_length()-1))
        if len(raw) < n:
            raise ValueError(f'Truncated PVR texture: {name}')
        raw = raw[:n]
        filename = f'{prefix}_{i:03d}_{name}.png'
        decode(raw, pixel, layout, w, h).save(image_dir/filename)
        textures.append(dict(name=name, image=filename, pixel=pixel, layout=layout, width=w, height=h,
                             raw=hardware_payload(raw, layout)))
        p += size + 8
    return textures
