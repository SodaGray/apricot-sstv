# SPDX-License-Identifier: GPL-3.0-only
"""Generate test images for the SSTV bench (PPM P6), at every mode size.

  python3 make_testcard.py outdir
writes testcard.ppm / photo.ppm (320x240) and testcard_WxH.ppm / photo_WxH.ppm
for 320x256, 512x400, 640x496 and 800x616."""
import sys, os, math, random
from PIL import Image, ImageDraw

def card(W, H):
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    sy = H / 240.0
    bars = [(255,255,255),(255,255,0),(0,255,255),(0,255,0),(255,0,255),(255,0,0),(0,0,255),(0,0,0)]
    bw = W // len(bars)
    for i, c in enumerate(bars):
        d.rectangle([i*bw, 0, (i+1)*bw-1, int(80*sy)-1], fill=c)
    for x in range(W):                       # grey ramp
        g = round(x * 255 / (W-1))
        d.line([x, int(80*sy), x, int(110*sy)-1], fill=(g, g, g))
    for x in range(W):                       # hue ramp
        h = x / W * 6; k = int(h) % 6; f = h - int(h)
        r, g, b = [(1,f,0),(1-f,1,0),(0,1,f),(0,1-f,1),(f,0,1),(1,0,1-f)][k]
        d.line([x, int(110*sy), x, int(130*sy)-1], fill=(int(r*255), int(g*255), int(b*255)))
    x = 0                                    # resolution wedges (not scored: above video bandwidth)
    for period in (16, 8, 6, 4, 3, 2):
        for i in range(W // 6 - 5):
            if (i // (period // 2 if period > 2 else 1)) % 2 == 0:
                d.line([x+i, int(130*sy), x+i, int(170*sy)-1], fill=(255,255,255))
        x += W // 6
    d.rectangle([0, int(170*sy), W-1, H-1], fill=(30, 60, 110))
    d.ellipse([int(W*0.62), int(172*sy), int(W*0.94), H-4], outline=(255,255,255), width=3, fill=(220, 90, 40))
    d.text((10, int(180*sy)), "APRICOT SSTV", fill=(255,255,255))
    d.text((10, int(200*sy)), "%dx%d" % (W, H), fill=(255,230,120))
    for i in range(0, H, 20):                # left-edge tick marks (slant check)
        d.line([0, i, 6, i], fill=(255, 0, 0))
    return im

def photo(W, H):
    random.seed(7)
    im = Image.new("RGB", (W, H)); px = im.load()
    sx, sy = W / 320.0, H / 240.0
    for y in range(H):
        for x in range(W):
            X, Y = x / sx, y / sy
            hill = 150 + 25*math.sin(X/37.0) + 12*math.sin(X/11.0+1)
            if Y < hill:
                t = Y / hill
                c = (int(90+80*t), int(140+70*t), int(230-20*t))
            else:
                t = (Y-hill)/(240-hill+1)
                n = random.randint(-12, 12)
                c = (int(40+60*t)+n, int(120-30*t)+n, int(40+10*t)+n)
            dx, dy = X-250, Y-50
            if dx*dx+dy*dy < 22*22: c = (255, 230, 120)
            px[x, y] = tuple(max(0, min(255, v)) for v in c)
    return im

out = sys.argv[1] if len(sys.argv) > 1 else "."
card(320, 240).save(os.path.join(out, "testcard.ppm"))
photo(320, 240).save(os.path.join(out, "photo.ppm"))
for W, H in ((320, 256), (512, 400), (640, 496), (800, 616)):
    card(W, H).save(os.path.join(out, "testcard_%dx%d.ppm" % (W, H)))
    photo(W, H).save(os.path.join(out, "photo_%dx%d.ppm" % (W, H)))
