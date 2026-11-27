# throbber_meteor_schedule.py - how the Retro Web throbber's meteor timing was chosen
# (docs/2026-11-21_THROBBER_GLOBE.md). Mirrors render_globe in src/web_toolbar.c.
# Run: python tools/throbber_meteor_schedule.py (minutes; pure Python).
# Search meteor start phases so no streak comes near the moon (same geometry
# as render_globe in src/web_toolbar.c, normalised to the art size).
import math, itertools
N = 30
def geom(art):
    S = 1.0
    D = (art - 4) / art
    o = (S - D) / 2
    cx = o + D * 127.5 / 256; cy = o + D * 128.5 / 256
    R = D * 106.5 / 256; Ro = D * 117.0 / 256
    return cx, cy, R, Ro, art
def moon(f, g):
    cx, cy, R, Ro, art = g
    th = 2 * math.pi * f / N - math.pi / 2
    t = -35 * math.pi / 180
    ua, ub, va, vb = math.cos(t), -math.sin(t), math.sin(t), math.cos(t)
    oa, ob = Ro * 1.30, Ro * 0.34
    mz = math.sin(th)
    mx = cx + oa * math.cos(th) * ua + ob * math.sin(th) * va
    my = cy + oa * math.cos(th) * ub + ob * math.sin(th) * vb
    mr = R * 0.25 * (1 + 0.12 * mz)
    hidden = mz <= 0 and math.hypot(mx - cx, my - cy) + mr < R
    return mx, my, mr, hidden
OFF = (-0.20, 0.22, 0.50)
def streak(f, k, ph):
    dx, dy = -0.80, 0.60
    s = ((f / N) + 1.0 - ph) % 1.0
    span, ln = 3.2, 0.60
    hx = 1.15 + OFF[k] * 0.60 + dx * span * s
    hy = -0.45 + OFF[k] * 0.80 + dy * span * s
    tx, ty = hx - dx * ln, hy - dy * ln          # the tail end
    return hx, hy, tx, ty
def seg_dist(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    t = max(0, min(1, ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy)))
    return math.hypot(px - ax - t * vx, py - ay - t * vy)
def visible(ax, ay, bx, by):
    for i in range(21):
        x = ax + (bx - ax) * i / 20; y = ay + (by - ay) * i / 20
        if 0 <= x <= 1 and 0 <= y <= 1: return True
    return False
def score(phs, g):
    worst = 9
    for f in range(N):
        mx, my, mr, hid = moon(f, g)
        for k, ph in enumerate(phs):
            hx, hy, tx, ty = streak(f, k, ph)
            if f == 0 and visible(hx, hy, tx, ty): return -1
            if hid or not visible(hx, hy, tx, ty): continue
            worst = min(worst, seg_dist(mx, my, hx, hy, tx, ty) - mr)
    return worst
best = None
steps = [i / N for i in range(N)]
ga, gb = geom(44), geom(60)
for phs in itertools.product(steps, repeat=3):
    sc = min(score(phs, ga), score(phs, gb))
    if best is None or sc > best[0]: best = (sc, phs)
print("best clearance (fraction of the art) %.3f phases %s" % (best[0], best[1]))
print("current", min(score((0.30, 0.62, 0.86), ga), score((0.30, 0.62, 0.86), gb)))
# when each meteor is on screen (frames), and the moon's place then
for k, ph in enumerate(best[1]):
    fr = [f for f in range(N) if visible(*streak(f, k, ph))]
    print("meteor", k, "frames", fr[0] if fr else None, "-", fr[-1] if fr else None)
print("--- spread")
def arrivals(phs):
    out = []
    for k, ph in enumerate(phs):
        fr = [f for f in range(N) if visible(*streak(f, k, ph))]
        if not fr: return None
        # the first frame after a gap (circular)
        first = [f for f in fr if (f - 1) % N not in fr]
        out.append(first[0])
    return out
cands = []
for phs in itertools.product(steps, repeat=3):
    sc = min(score(phs, ga), score(phs, gb))
    if sc < 0.12: continue
    a = arrivals(phs)
    if not a: continue
    a2 = sorted(a)
    gaps = [(a2[(i + 1) % 3] - a2[i]) % N for i in range(3)]
    cands.append((min(gaps), sc, phs, a))
cands.sort(key=lambda c: (c[0], c[1]), reverse=True)
for c in cands[:5]: print("min gap %d frames, clearance %.3f, phases %s, arrivals %s" % c)
