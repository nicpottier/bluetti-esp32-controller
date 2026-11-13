#!/usr/bin/env python3
"""Mockup of the LCARS UI on the 3.5" 320x480 display (portrait and landscape).

Draws at 2x and downsamples, using the same Antonio font and palette as the
firmware. Run from the project root. Usage: mock_rect.py OUT.png
"""
import sys

from PIL import Image, ImageDraw, ImageFont

FONT = "fonts/Antonio.ttf"
S = 2  # supersample
RADIUS = 13  # every rounded corner, as in the firmware; 26 px bars become pills

BLACK = (0, 0, 0)
FRAME = (245, 158, 11)
BAR = (71, 85, 105)
AC = (56, 189, 248)
DC = (167, 139, 250)
LABEL = (148, 163, 184)
EMPTY = (30, 41, 59)
OFF = (51, 65, 85)
TEXT = (241, 245, 249)


def font(size, weight=600):
    f = ImageFont.truetype(FONT, size * S)
    f.set_variation_by_axes([weight])
    return f


def soc_color(soc):
    stops = [(0, (244, 63, 94)), (20, (249, 115, 22)), (45, (251, 191, 36)), (75, (52, 211, 153)), (100, (52, 211, 153))]
    for (a, ca), (b, cb) in zip(stops, stops[1:]):
        if soc <= b:
            t = (soc - a) / (b - a)
            return tuple(round(x + (y - x) * t) for x, y in zip(ca, cb))
    return stops[-1][1]


def T(d, s, x, y, f, fill, anchor):
    d.text((x * S, y * S), s, font=f, fill=fill, anchor=anchor)


def box(d, x0, y0, x1, y1, fill, r=0, corners=(True, True, True, True)):
    d.rounded_rectangle([x0 * S, y0 * S, x1 * S, y1 * S], radius=r * S, fill=fill, corners=corners)


def gauge(d, x0, y0, x1, y1, soc, color):
    segs, gap = 20, 3
    w = (x1 - x0 - gap * (segs - 1)) / segs
    lit = soc * segs // 100 or (1 if soc > 0 else 0)
    for i in range(segs):
        sx = x0 + i * (w + gap)
        first, last = i == 0, i == segs - 1
        box(d, sx, y0, sx + w, y1, color if i < lit else EMPTY, r=(y1 - y0) / 2 if first or last else 0,
            corners=(first, last, last, first))


def watts(d, x, y, w, vf, uf, col, anchor="m"):
    v = str(w)
    vw = d.textlength(v, font=vf) / S
    uw = d.textlength("W", font=uf) / S
    tot = vw + 3 + uw
    x0 = {"m": x - tot / 2, "r": x - tot, "l": x}[anchor]
    T(d, v, x0, y, vf, col, "ls")
    T(d, "W", x0 + vw + 3, y, uf, col, "ls")


def charge(d, cx, base, soc, value, unit, accent, big=150):
    bf, pf = font(big), font(48)
    s_w = d.textlength(str(soc), font=bf) / S
    p_w = d.textlength("%", font=pf) / S
    x = cx - (s_w + 4 + p_w) / 2
    T(d, str(soc), x, base, bf, accent, "ls")
    T(d, "%", x + s_w + 4, base, pf, accent, "ls")
    tv, tu = font(44, 500), font(22)
    v_w = d.textlength(value, font=tv) / S
    u_w = d.textlength(unit, font=tu) / S
    x = cx - (v_w + 8 + u_w) / 2
    T(d, value, x, base + 50, tv, TEXT, "ls")
    T(d, unit, x + v_w + 8, base + 50, tu, accent, "ls")


def inputs(d, cols, ylab, yval, ac_in, dc_in):
    lf, vf, uf = font(22), font(40), font(22)
    for cx, label, col, w in ((cols[0], "AC IN", AC, ac_in), (cols[1], "DC IN", DC, dc_in)):
        T(d, label, cx, ylab, lf, col, "ms")
        watts(d, cx, yval, w, vf, uf, TEXT if w else LABEL)


def out_button(d, x0, y0, x1, y1, label, on, col, w, outer_left):
    # Rounded on the outer side, square where the two buttons meet
    corners = (True, False, False, True) if outer_left else (False, True, True, False)
    box(d, x0, y0, x1, y1, col if on else OFF, r=RADIUS, corners=corners)
    ink = BLACK if on else LABEL
    base = y1 - 16
    T(d, label, x0 + 18 if outer_left else x0 + 14, base, font(40, 700), ink, "ls")
    watts(d, x1 - 14 if outer_left else x1 - 18, base, w, font(30), font(20), ink, "r")


def status_bar(d, x0, y0, x1, y1, status, device, color=BAR):
    box(d, x0, y0, x1, y1, color, r=(y1 - y0) / 2)
    f = font(17)
    mid = (y0 + y1) / 2 + 6
    T(d, status, x0 + 16, mid, f, BLACK, "ls")
    T(d, device, x1 - 16, mid, f, BLACK, "rs")


DEFAULT = dict(soc=91, value="19.8", unit="DAYS", ac_on=False, dc_on=True, readings=(0, 0, 0, 0),
               status="LINK ACTIVE", device="ELITE 300")


def portrait(**kw):
    a = {**DEFAULT, **kw}
    ac_in, ac_out, dc_in, dc_out = a["readings"]
    W, H = 320, 480
    im = Image.new("RGB", (W * S, H * S), BLACK)
    d = ImageDraw.Draw(im)
    accent = soc_color(a["soc"])
    gauge(d, 10, 10, 310, 36, a["soc"], accent)
    charge(d, W / 2, 196, a["soc"], a["value"], a["unit"], accent)
    inputs(d, (85, 235), 290, 334, ac_in, dc_in)
    out_button(d, 10, 354, 157, 434, "AC", a["ac_on"], AC, ac_out, True)
    out_button(d, 163, 354, 310, 434, "DC", a["dc_on"], DC, dc_out, False)
    status_bar(d, 10, 444, 310, 470, a["status"], a["device"])
    return im.resize((W, H), Image.LANCZOS)


def landscape(**kw):
    a = {**DEFAULT, **kw}
    ac_in, ac_out, dc_in, dc_out = a["readings"]
    W, H = 480, 320
    im = Image.new("RGB", (W * S, H * S), BLACK)
    d = ImageDraw.Draw(im)
    accent = soc_color(a["soc"])
    gauge(d, 10, 10, 470, 36, a["soc"], accent)
    charge(d, 125, 176, a["soc"], a["value"], a["unit"], accent)
    inputs(d, (305, 415), 72, 114, ac_in, dc_in)
    # stacked outputs on the right: rounded on the right edge, LCARS sidebar style
    for y0, label, on, col, w in ((134, "AC", a["ac_on"], AC, ac_out), (206, "DC", a["dc_on"], DC, dc_out)):
        box(d, 250, y0, 470, y0 + 66, col if on else OFF, r=26, corners=(False, True, True, False))
        ink = BLACK if on else LABEL
        T(d, label, 266, y0 + 50, font(40, 700), ink, "ls")
        watts(d, 452, y0 + 50, w, font(30), font(20), ink, "r")
    status_bar(d, 10, 284, 470, 310, a["status"], a["device"])
    return im.resize((W, H), Image.LANCZOS)


def picker(rows, cursor):
    W, H = 320, 480
    im = Image.new("RGB", (W * S, H * S), BLACK)
    d = ImageDraw.Draw(im)
    box(d, 10, 10, 310, 36, FRAME, r=13)
    T(d, "SELECT STATION", 26, 30, font(20), BLACK, "ls")
    y = 52
    for i, (model, sn) in enumerate(rows):
        sel = i == cursor
        box(d, 10, y, 310, y + 84, FRAME if sel else EMPTY, r=RADIUS)
        T(d, model, 32, y + 46, font(36), BLACK if sel else TEXT, "ls")
        T(d, sn, 32, y + 72, font(18), BLACK if sel else LABEL, "ls")
        y += 92
    status_bar(d, 10, 444, 310, 470, "TAP TO CONNECT", "2 FOUND", FRAME)
    return im.resize((W, H), Image.LANCZOS)


def sheet(screens, path):
    pad, ring = 36, 12
    W = sum(s.width + 2 * ring for s in screens) + pad * (len(screens) + 1)
    H = max(s.height for s in screens) + 2 * ring + 2 * pad
    out = Image.new("RGB", (W, H), (58, 58, 62))
    d = ImageDraw.Draw(out)
    x = pad
    for s in screens:
        d.rounded_rectangle([x, pad, x + s.width + 2 * ring, pad + s.height + 2 * ring], radius=14, fill=(18, 18, 20))
        out.paste(s, (x + ring, pad + ring))
        x += s.width + 2 * ring + pad
    out.save(path)


if __name__ == "__main__":
    on = dict(soc=34, value="5.4", unit="HOURS", ac_on=True, readings=(0, 182, 96, 14))
    sheet([
        portrait(),
        portrait(**on),
        picker([("ELITE 300", "1234567890123"), ("HANDSFREE 1", "9876543210987")], 0),
        landscape(**on),
    ], sys.argv[1])
