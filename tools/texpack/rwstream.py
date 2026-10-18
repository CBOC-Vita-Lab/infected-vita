"""Reader for the level/front-end streams of the PSP game (RenderWare 3.7 chunk streams).

A stream file (`*.stream.zpm`) is a little-endian inflated-size word followed by a zlib stream. The inflated data is a
game-specific serialisation that embeds ordinary RenderWare chunks; the ones this tool wants are the texture
dictionaries (chunk 0x16) with their PSP-native rasters (chunk 0x15).

Native raster (chunk 0x15 -> struct chunk 0x01), offsets from the start of the struct data:
    +0x00 u32  platform word (0x25/0x26/0x45/0x46/0xA6/0xC5/0xC6 << 8: wrap/filter flags, not the pixel format)
    +0x04 u16 width, u16 height
    +0x08 u8 bits per texel (4, 8, or 16 for direct RGB565), u8 mip levels, u8 4, u8 0x7C or 0
    +0x6c char name[32], +0x8c char mask[32]
    +0xac palette: 16 (CLUT4) or 256 (CLUT8) RGBA8888 entries
    then the level 0 texels in the PSP's swizzled layout (width*height*bits/8 bytes), then the smaller levels.
"""
import re
import struct
import zlib
from dataclasses import dataclass
from typing import Iterator, List

STAMP = b'\x55\x00\x02\x1c'  # library id 0x1C020055 inside the stream's embedded chunks
RW_TEXDICTIONARY = 0x16
RW_TEXTURENATIVE = 0x15
RW_STRUCT = 0x01
RW_EXTENSION = 0x03

_TXD = re.compile(rb'\x16\x00\x00\x00(....)\x55\x00\x02\x1c\x01\x00\x00\x00\x04\x00\x00\x00\x55\x00\x02\x1c(..)(..)', re.S)


@dataclass
class Raster:
    name: str
    mask: str
    width: int
    height: int
    bits: int
    levels: int
    platform: int
    palette: bytes   # RGBA8888 entries in memory order
    texels: bytes    # level 0, swizzled, as the GE addresses it
    offset: int      # offset of the native chunk in the inflated stream
    txd: int         # offset of its texture dictionary chunk


def load_stream(path: str) -> bytes:
    with open(path, 'rb') as f:
        data = f.read()
    size = struct.unpack_from('<I', data, 0)[0]
    out = zlib.decompressobj().decompress(data[4:])
    if len(out) != size:
        raise ValueError(f'{path}: inflated {len(out)} bytes, header says {size}')
    return out


def chunk(buf: bytes, off: int):
    t, s, v = struct.unpack_from('<III', buf, off)
    return t, s, v


def txd_chunks(buf: bytes) -> Iterator[int]:
    for m in _TXD.finditer(buf):
        yield m.start()


def parse_txd(buf: bytes, off: int) -> List[Raster]:
    t, size, _ = chunk(buf, off)
    assert t == RW_TEXDICTIONARY
    end = off + 12 + size
    pos = off + 12
    st, ssz, _ = chunk(buf, pos)
    assert st == RW_STRUCT
    count, device = struct.unpack_from('<HH', buf, pos + 12)
    pos += 12 + ssz
    out: List[Raster] = []
    while pos + 12 <= end and len(out) < count:
        t, s, _ = chunk(buf, pos)
        if t != RW_TEXTURENATIVE:
            break
        out.append(parse_native(buf, pos, off))
        pos += 12 + s
    if len(out) != count:
        raise ValueError(f'txd at {off:#x}: expected {count} rasters, parsed {len(out)}')
    return out


def parse_native(buf: bytes, off: int, txd: int) -> Raster:
    t, size, _ = chunk(buf, off)
    st, ssz, _ = chunk(buf, off + 12)
    assert st == RW_STRUCT, hex(st)
    d = off + 24
    platform, = struct.unpack_from('<I', buf, d)
    width, height = struct.unpack_from('<HH', buf, d + 4)
    bits, levels = buf[d + 8], buf[d + 9]
    if bits not in (4, 8, 16):
        raise ValueError(f'raster at {off:#x}: {bits} bits per texel')
    name = buf[d + 0x6c:d + 0x8c].split(b'\0')[0].decode('latin1')
    mask = buf[d + 0x8c:d + 0xac].split(b'\0')[0].decode('latin1')
    entries = {4: 16, 8: 256, 16: 0}[bits]   # 16-bit rasters (six subway signs) are direct RGB565, no palette
    pal = bytes(buf[d + 0xac:d + 0xac + entries * 4])
    tex_off = d + 0xac + entries * 4
    tex_len = width * height * bits // 8
    if tex_off + tex_len > d + ssz:
        raise ValueError(f'raster {name} at {off:#x} overruns its chunk')
    return Raster(name, mask, width, height, bits, levels, platform, pal, bytes(buf[tex_off:tex_off + tex_len]), off, txd)


def rasters_in(path: str) -> List[Raster]:
    buf = load_stream(path)
    out: List[Raster] = []
    for t in txd_chunks(buf):
        out.extend(parse_txd(buf, t))
    return out


def raw_record(r: Raster) -> bytes:
    """The record format of the renderer's texture dump and of `texkey batch` (IFRW)."""
    fmt = {4: 4, 8: 5, 16: 0}[r.bits]
    clut_format = 3 if r.bits != 16 else 0
    return (b'IFRW' + struct.pack('<I', 1) + struct.pack('<BBBB', fmt, clut_format, 1, 0) +
            struct.pack('<6H', r.width, r.height, r.width, 0, 255, 0) +
            struct.pack('<II', len(r.texels), len(r.palette)) + r.texels + r.palette)
