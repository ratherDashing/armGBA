#!/usr/bin/env python3
# Draws icon.png (48x48) and banner.png (256x128) and writes banner.wav, the HOME Menu's chime.
# Needs Pillow and a bold sans font (DejaVu Sans Bold). Run from platform/3ds: python3 art/make.py
import math, struct, wave
from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
NAVY, NAVY2, BLUE, WHITE, RED, GREY = (22, 27, 46), (36, 44, 74), (96, 165, 250), (245, 247, 252), (239, 68, 68), (148, 163, 184)

def font(size):
    return ImageFont.truetype(FONT, size)

def centered(d, box, text, f, fill):
    l, t, r, b = d.textbbox((0, 0), text, font=f)
    d.text((box[0] + (box[2] - box[0] - (r - l)) / 2 - l, box[1] + (box[3] - box[1] - (b - t)) / 2 - t), text, font=f, fill=fill)

# The mark: a chip, its pins along the sides, "arm" over "GBA", and a red "3DS" tab.
def mark(size):
    s = size
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    pin = s * 0.055
    for i in range(5):
        y = s * (0.2 + i * 0.15)
        d.rounded_rectangle((0, y - pin / 2, s, y + pin / 2), pin / 2, fill=GREY)
    d.rounded_rectangle((s * 0.07, s * 0.04, s * 0.93, s * 0.96), s * 0.16, fill=NAVY, outline=BLUE, width=max(1, round(s * 0.03)))
    centered(d, (s * 0.07, s * 0.10, s * 0.93, s * 0.42), "arm", font(round(s * 0.30)), BLUE)
    centered(d, (s * 0.07, s * 0.38, s * 0.93, s * 0.70), "GBA", font(round(s * 0.30)), WHITE)
    d.rounded_rectangle((s * 0.27, s * 0.72, s * 0.73, s * 0.89), s * 0.05, fill=RED)
    centered(d, (s * 0.27, s * 0.72, s * 0.73, s * 0.89), "3DS", font(round(s * 0.13)), WHITE)
    return im

def icon():
    big = Image.new("RGBA", (384, 384), NAVY2 + (255,))
    m = mark(352)
    big.paste(m, (16, 16), m)
    big.resize((48, 48), Image.LANCZOS).save("icon.png")

def banner():
    k = 4
    im = Image.new("RGBA", (256 * k, 128 * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((6 * k, 10 * k, 250 * k, 118 * k), 18 * k, fill=NAVY2 + (255,), outline=BLUE, width=2 * k)
    m = mark(92 * k)
    im.paste(m, (16 * k, 18 * k), m)
    d.text((116 * k, 34 * k), "armGBA", font=font(27 * k), fill=WHITE)
    d.rounded_rectangle((117 * k, 70 * k, 171 * k, 90 * k), 5 * k, fill=RED)
    centered(d, (117 * k, 70 * k, 171 * k, 90 * k), "3DS", font(15 * k), WHITE)
    d.text((117 * k, 97 * k), "GBA · GB · GBC", font=font(10 * k), fill=GREY)
    im.resize((256, 128), Image.LANCZOS).save("banner.png")

# Two soft bell notes a fifth apart, then the octave: 1.6 s, fading to nothing.
def chime():
    rate = 32000
    notes = [(0.00, 659.26), (0.16, 987.77), (0.32, 1318.51)]
    n = int(rate * 1.6)
    out = [0.0] * n
    for start, freq in notes:
        for i in range(int(start * rate), n):
            t = (i - start * rate) / rate
            env = min(1.0, t / 0.006) * math.exp(-t * 4.2)
            out[i] += env * (math.sin(2 * math.pi * freq * t) + 0.28 * math.sin(4 * math.pi * freq * t) * math.exp(-t * 6) + 0.10 * math.sin(6 * math.pi * freq * t) * math.exp(-t * 9))
    peak = max(abs(v) for v in out)
    fade = int(rate * 0.15)
    with wave.open("banner.wav", "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(v / peak * 0.55 * 32767 * min(1.0, (n - i) / fade))) for i, v in enumerate(out)))

icon(); banner(); chime()
