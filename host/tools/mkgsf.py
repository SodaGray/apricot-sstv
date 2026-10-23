# SPDX-License-Identifier: GPL-3.0-only
"""mkgsf.py -- write a GSF1 bitmap font (the format gsf.c reads) from a
TrueType/OpenType font, for testing the iPod-side renderer on the host.

  python3 mkgsf.py font.ttf size out.gsf [--chars "..."] [--family NAME]

Layout (little-endian), as gsf.c validates it:
  header 128 B: "GSF1", u16 version=1, u16 header=128, u32 flags
  (MSB_FIRST|TIGHT_BITS|SORTED_CODEPOINTS), u16 point_size, u16 line_height,
  s16 base_line (from the bottom of the line), u8 bpp=1, u32 glyph_count @20,
  u32 table_offset @24, u32 record_size=16 @28, u32 bitmap_offset @32,
  u32 bitmap_size @36, u32 first_cp @40, u32 last_cp @44,
  u32 crc32(table..bitmap end) @48, family[32] @52,
  style[16] @84, source[28] @100.
  glyph record 16 B: u32 codepoint, u32 bitmap_rel, u16 advance, u8 box_w,
  u8 box_h, s8 ofs_x, s8 ofs_y (box bottom above the baseline), u16 bitmap_size.
  bitmaps: 1 bpp, MSB first, rows packed without padding.
"""
import struct, sys, zlib
from PIL import Image, ImageDraw, ImageFont


def build(ttf, size, chars, family, style="Regular"):
    font = ImageFont.truetype(ttf, size)
    ascent, descent = font.getmetrics()
    recs, bitmaps = [], bytearray()
    for cp in sorted(set(ord(c) for c in chars)):
        ch = chr(cp)
        adv = int(round(font.getlength(ch)))
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        bits = []
        if w and h:
            im = Image.new("1", (w, h), 0)
            d = ImageDraw.Draw(im)
            d.fontmode = "1"
            d.text((-x0, -y0), ch, font=font, fill=1, anchor="ls")
            px = im.load()
            bits = [1 if px[x, y] else 0 for y in range(h) for x in range(w)]
        packed = bytearray((len(bits) + 7) // 8)
        for i, b in enumerate(bits):
            if b:
                packed[i >> 3] |= 0x80 >> (i & 7)
        recs.append((cp, len(bitmaps), adv, w, h, x0, -y1, len(packed)))
        bitmaps += packed
    table_off = 128
    bitmap_off = table_off + 16 * len(recs)
    table = b"".join(struct.pack("<IIHBBbbH", *r) for r in recs)
    crc = zlib.crc32(table + bytes(bitmaps)) & 0xFFFFFFFF
    hdr = bytearray(128)
    hdr[0:4] = b"GSF1"
    struct.pack_into("<HHIHHhB", hdr, 4, 1, 128, 0x7, size, ascent + descent, descent, 1)
    struct.pack_into("<IIIII", hdr, 20, len(recs), table_off, 16, bitmap_off, len(bitmaps))
    struct.pack_into("<II", hdr, 40, recs[0][0], recs[-1][0])   # first/last codepoint
    struct.pack_into("<I", hdr, 48, crc)
    hdr[52:52 + 32] = family.encode()[:31].ljust(32, b"\0")
    hdr[84:84 + 16] = style.encode()[:15].ljust(16, b"\0")
    hdr[100:128] = b"mkgsf.py".ljust(28, b"\0")
    return bytes(hdr) + table + bytes(bitmaps)


if __name__ == "__main__":
    args = sys.argv[1:]
    chars = "".join(chr(c) for c in range(0x20, 0x7F))
    family = "Test"
    if "--chars" in args:
        i = args.index("--chars"); chars = args[i + 1]; del args[i:i + 2]
    if "--family" in args:
        i = args.index("--family"); family = args[i + 1]; del args[i:i + 2]
    data = build(args[0], int(args[1]), chars, family)
    open(args[2], "wb").write(data)
    print("%s: %d glyphs, %d bytes" % (args[2], len(set(chars)), len(data)))
