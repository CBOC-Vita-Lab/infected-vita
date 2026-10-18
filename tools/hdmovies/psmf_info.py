#!/usr/bin/env python3
"""Reads the header of a PSMF/PMF movie and prints what the HD movie tools and the runtime need.

    psmf_info.py FILE.pmf            ->  frames width height crc32(header) stream_bytes start end
    psmf_info.py --json FILE.pmf

The header is the first 0x800 bytes (sceMpegQueryStreamOffset: the offset at +8 of the header is 2048 on every movie of the
game). Layout, all big-endian: "PSMF", version, +8 header size, +12 stream size, +0x54 start time and +0x5A end time (48 bit,
90 kHz). The video is 29.97 fps (3003 ticks a frame), so (end - start) / 3003 is the number of pictures. The picture size is in
the first video entry of the stream table (0x82 + 16 * i, +12 and +13 in 16-pixel units).
The CRC-32 is the one the runtime computes over the first 2048 bytes: it is how a movie is recognised.
"""
import json
import struct
import sys
import zlib

HEADER_BYTES = 2048
TICKS_PER_FRAME = 3003


def read_info(path):
    with open(path, "rb") as f:
        head = f.read(HEADER_BYTES)
    if len(head) < HEADER_BYTES or head[:4] != b"PSMF":
        raise ValueError("%s: not a PSMF movie" % path)
    header_size, stream_size = struct.unpack(">II", head[8:16])
    start = int.from_bytes(head[0x54:0x5A], "big")
    end = int.from_bytes(head[0x5A:0x60], "big")
    count = struct.unpack(">H", head[0x80:0x82])[0]
    width = height = 0
    for i in range(count):
        entry = head[0x82 + 16 * i: 0x82 + 16 * i + 16]
        if 0xE0 <= entry[0] <= 0xEF:
            width, height = entry[12] * 16, entry[13] * 16
            break
    frames, rest = divmod(end - start, TICKS_PER_FRAME)
    return {
        "frames": frames,
        "tick_remainder": rest,
        "width": width,
        "height": height,
        "crc32": "%08x" % (zlib.crc32(head) & 0xFFFFFFFF),
        "stream_bytes": stream_size,
        "header_bytes": header_size,
        "start": start,
        "end": end,
    }


def main(argv):
    as_json = "--json" in argv
    files = [a for a in argv if a != "--json"]
    if len(files) != 1:
        sys.stderr.write(__doc__)
        return 2
    info = read_info(files[0])
    if as_json:
        print(json.dumps(info))
    else:
        print(info["frames"], info["width"], info["height"], info["crc32"], info["stream_bytes"], info["start"], info["end"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
