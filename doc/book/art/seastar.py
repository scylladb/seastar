#!/usr/bin/python3 -I
# Draw the sea star for the title page, in the style of a pen-and-ink
# engraving, as SVG. The body is a smooth union of a central disc and five
# tapered arms; it is shaded by pen lines that run along each arm and thicken
# where the surface turns away from the light (from the upper left).
# The title page uses the committed seastar.svg, made with
#   seastar.py stipple 7 > seastar.svg
# and the build doesn't run this script, so the artwork can't change by
# accident (a newer Python or math library could change the random numbers or
# the rounding). Run it only to make a new version deliberately, and commit
# the result.
#
# Usage: seastar.py STYLE SEED > out.svg
#   STYLE: engraving (lines along the arms), stipple (dots), or line (no shading)

import math
import random
import sys

style, seed = sys.argv[1], int(sys.argv[2])
rng = random.Random(seed)
W = H = 600
cx, cy = W / 2, H / 2 + 12
R_disc = 62.0
arms = 5
base = -math.pi / 2
phis = [base + k * 2 * math.pi / arms + rng.uniform(-0.06, 0.06) for k in range(arms)]
lengths = [rng.uniform(232, 262) for _ in range(arms)]
bends = [rng.uniform(-0.22, 0.22) for _ in range(arms)]
W0, W_tip = 52.0, 5.5
light = (-0.48, -0.58, 0.66)             # x right, y down, z toward the viewer
ln = math.sqrt(sum(c * c for c in light))
light = tuple(c / ln for c in light)
out = []
# Stipple dots, by radius: written at the end as one path per radius, each dot
# a zero-length stroke with round caps, which keeps the SVG small.
dots = {}

def dot(x, y, r):
    dots.setdefault(round(r, 1), []).append(f'M{x:.1f} {y:.1f}h0')

def width(t):
    return W_tip + (W0 - W_tip) * (1 - t) ** 0.9

def sd_uneven_capsule(px, py, r1, r2, h):
    # Signed distance to a capsule along +y from radius r1 at 0 to r2 at h.
    px = abs(px)
    b = (r1 - r2) / h
    a = math.sqrt(1 - b * b)
    k = -b * px + a * py
    if k < 0:
        return math.hypot(px, py) - r1
    if k > a * h:
        return math.hypot(px, py - h) - r2
    return a * px + b * py - r1

def smin(a, b, k=16.0):
    h = max(k - abs(a - b), 0.0) / k
    return min(a, b) - h * h * k * 0.25

def unbend(x, y):
    # The inverse of bend(): straighten the arms before measuring.
    r, th = math.hypot(x, y), math.atan2(y, x)
    k = nearest_arm(th)
    return r * math.cos(th - bends[k] * (r / lengths[k]) ** 2), r * math.sin(th - bends[k] * (r / lengths[k]) ** 2)

def bend(x, y):
    r, th = math.hypot(x, y), math.atan2(y, x)
    k = nearest_arm(th)
    th += bends[k] * (r / lengths[k]) ** 2
    return r * math.cos(th), r * math.sin(th)

def nearest_arm(th):
    return min(range(arms), key=lambda k: abs(math.remainder(th - phis[k], 2 * math.pi)))

def field(x, y):
    # Negative inside the sea star (in unbent coordinates).
    d = math.hypot(x, y) - R_disc
    for k in range(arms):
        c, s = math.cos(phis[k]), math.sin(phis[k])
        ax, ay = x * c + y * s, -x * s + y * c     # along the arm, across it
        d = smin(d, sd_uneven_capsule(ay, ax, W0, W_tip, lengths[k] - W_tip))
    return d

def P(x, y):
    bx, by = bend(x, y)
    return cx + bx, cy + by

def boundary(th):
    lo, hi = 0.0, 300.0
    for _ in range(40):
        mid = (lo + hi) / 2
        if field(mid * math.cos(th), mid * math.sin(th)) < 0:
            lo = mid
        else:
            hi = mid
    return lo

# Pen strokes, by width: written at the end as one path per width.
strokes = {}

def stroke(pts, w):
    d = 'M' + 'L'.join(f'{x:.1f} {y:.1f}' for x, y in pts)
    strokes.setdefault(round(w, 1), []).append(d)

def darkness(nx, ny, nz):
    b = max(0.0, nx * light[0] + ny * light[1] + nz * light[2])
    return 1 - b

# The outline, heavier on the shadow side, as engravers draw it.
n = 1800
outline = []
for i in range(n + 1):
    th = 2 * math.pi * i / n
    r = boundary(th)
    outline.append((th, r, *P(r * math.cos(th), r * math.sin(th))))
for i in range(n):
    th, r, x0, y0 = outline[i]
    _, _, x1, y1 = outline[i + 1]
    tx, ty = x1 - x0, y1 - y0
    L = math.hypot(tx, ty) or 1
    nx, ny = ty / L, -tx / L
    if nx * (x0 - cx) + ny * (y0 - cy) < 0:
        nx, ny = -nx, -ny
    dk = darkness(nx * 0.9, ny * 0.9, 0.3)
    stroke([(x0, y0), (x1, y1)], 1.1 + 1.6 * dk ** 2)

def arm_point(k, t, u):
    # A point on arm k at fraction t of its length and lateral position u
    # (-1 to 1 across the arm), in unbent coordinates.
    c, s = math.cos(phis[k]), math.sin(phis[k])
    along, across = t * lengths[k], u * width(t) * 0.93
    return along * c - across * s, along * s + across * c

def arm_normal(k, u):
    c, s = math.cos(phis[k]), math.sin(phis[k])
    side = (-s, c)
    return side[0] * u, side[1] * u, math.sqrt(max(0.0, 1 - u * u))

# Shading of the arms.
if style == 'engraving':
    for k in range(arms):
        t0 = (R_disc * 0.95) / lengths[k]
        for u in [i / 13 for i in range(-12, 13)]:
            dk = darkness(*arm_normal(k, u))
            if dk < 0.32:
                continue
            w = 0.25 + 1.25 * ((dk - 0.32) / 0.68) ** 1.4
            pts = []
            t = t0 + abs(u) * 0.04
            while t < 0.985:
                x, y = arm_point(k, t, u)
                if field(x, y) < -1.0:
                    pts.append(P(x, y))
                t += 0.01
            if len(pts) > 2:
                stroke(pts, w * (0.75 + 0.25 * (1 - abs(u))))
            # Cross-hatching in the darkest band.
            if dk > 0.78 and round(u * 13) % 2 == 0:
                t = t0 + 0.03
                while t < 0.9:
                    a = arm_point(k, t, u - 0.06)
                    b = arm_point(k, t + 0.012, u + 0.06)
                    if field(*a) < -1 and field(*b) < -1:
                        stroke([P(*a), P(*b)], 0.35)
                    t += 0.022
elif style == 'stipple':
    for k in range(arms):
        t0 = (R_disc * 0.95) / lengths[k]
        for _ in range(5200):
            t = rng.uniform(t0, 0.98)
            u = rng.uniform(-1, 1)
            dk = darkness(*arm_normal(k, u))
            if rng.random() < max(0.0, (dk - 0.3) / 0.7) ** 1.6:
                x, y = arm_point(k, t, u)
                if field(x, y) < -1.0:
                    px, py = P(x, y)
                    dot(px, py, rng.uniform(0.45, 0.85))

# The central disc, stippled as a dome.
if style != 'line':
    for _ in range(1400):
        r = R_disc * math.sqrt(rng.random())
        th = rng.uniform(0, 2 * math.pi)
        x, y = r * math.cos(th), r * math.sin(th)
        q = r / (R_disc * 1.15)
        dk = darkness(x / R_disc * 0.8, y / R_disc * 0.8, math.sqrt(max(0, 1 - q * q)))
        if rng.random() < max(0.0, (dk - 0.3) / 0.7) ** 1.5 * 0.7:
            px, py = P(x, y)
            dot(px, py, 0.6)

def knob(x, y, size, shaded):
    px, py = P(x, y)
    out.append(f'<ellipse cx="{px:.2f}" cy="{py:.2f}" rx="{size:.2f}" ry="{size * 0.9:.2f}" '
               f'fill="#fff" stroke="#111" stroke-width="{0.5 + size * 0.07:.2f}"/>')
    if shaded:
        # A crescent of shadow on the side away from the light.
        out.append(f'<path d="M{px + size * 0.15:.2f},{py + size * 0.9:.2f} '
                   f'A{size:.2f},{size * 0.9:.2f} 0 0 0 {px + size * 0.98:.2f},{py + size * 0.05:.2f}" '
                   f'fill="none" stroke="#111" stroke-width="{0.4 + size * 0.22:.2f}"/>')

# Knobs: a ridge of larger ones along each arm, looser rows beside it,
# and small spines along the edges.
for k in range(arms):
    t = (R_disc * 0.55) / lengths[k]
    while t < 0.93:
        size = 5.2 * (1 - 0.7 * t)
        u = rng.uniform(-0.05, 0.05)
        knob(*arm_point(k, t, u), size, style != 'line')
        t += size * 2.5 / lengths[k] * rng.uniform(0.9, 1.15)
    for side in (-1, 1):
        t = (R_disc * 0.9) / lengths[k] + rng.uniform(0, 0.03)
        while t < 0.88:
            size = 3.2 * (1 - 0.7 * t)
            u = side * rng.uniform(0.45, 0.6)
            if rng.random() < 0.85:
                knob(*arm_point(k, t, u), size, style != 'line')
            t += size * 3.4 / lengths[k] * rng.uniform(0.8, 1.3)
    for side in (-1, 1):
        t = (R_disc * 1.0) / lengths[k]
        while t < 0.95:
            a = arm_point(k, t, side * 0.98)
            b = arm_point(k, t, side * 1.18)
            stroke([P(*a), P(*b)], 0.6)
            t += rng.uniform(0.018, 0.03)

# The disc's ring of plates. (The madreporite, the sieve plate beside the
# center, is left out: at title-page size it looks like a blot.)
for i in range(10):
    th = base + math.pi / arms + i * 2 * math.pi / 10
    knob(R_disc * 0.6 * math.cos(th), R_disc * 0.6 * math.sin(th), 3.6, style != 'line')
print(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">')
for w, ds in sorted(strokes.items()):
    print(f'<path d="{"".join(ds)}" fill="none" stroke="#111" stroke-width="{w:.1f}" '
          'stroke-linecap="round" stroke-linejoin="round"/>')
for r, ds in sorted(dots.items()):
    print(f'<path d="{"".join(ds)}" stroke="#111" stroke-width="{2 * r:.1f}" stroke-linecap="round"/>')
print('\n'.join(out))
print('</svg>')
