#!/usr/bin/env python3
# Checks the frames picom drew during workspace switches, from picom's frame timing log
# with the layout records: PICOM_DEBUG="frame_timing=<path>;frame_layout=1" (see
# src/frame_timing.h in picom). The log says, for each frame, where each window is drawn
# (its four corners), with what opacity and crop, and the values of its animation
# script.
#
# Time is picom's animation clock. Within an animation a frame's time is the elapsed time
# the log gives, which the script saw (to the microsecond in the picom fork, in whole
# milliseconds of the frame's start in picom releases), and which stands still while
# picom holds the animations (while the screen pauses). Where an animation starts, the
# frame's start rounded down to the millisecond places it among the others: with the
# fork's clock up to 1 ms off, which the allowance at interruptions below covers. On that
# clock every pose is an exact function of time, and the checks below need no allowance
# for frame timing. Frames in which picom
# held an animation are reported as information.
#
# Continuity. A window's motion is smooth within an animation, and may change its speed
# where an animation starts or ends. So for every step from one frame to the next, the
# frames on each side, of the same animation, predict where the window is during the
# step: a range from the straight line through the two nearest frames to the curve
# through three, widened by the next term from a fourth (the bend keeps up, or eases
# off). The two must meet at some time within the step, within half a pixel (what a
# screen can't show) and the log's rounding. They may meet at an angle: that is a change
# of speed, not of place. A step one of whose ends lies on the other side's piece passes
# too: a bend in the step next to it, which the frames of one side straddle, is checked
# in that step. So does a step in which each value moves by what the slowest and the
# fastest speed the two sides show at its ends (with their errors) allow: a bend
# followed by more bending, which neither side's curve predicts; a jump moves a value
# further than that. Where an animation interrupts a running one, picom logs the old one
# as it hands over (an X record): the new one starts from exactly that pose, at its
# elapsed 0. The old one reaches it from its last frame through a part no frame shows,
# where its script may bend (a max that switches), so with twice the step its frames
# make as allowance; for a crop edge, which such a bend can start, the step of any of
# the window's corners and crop edges. An animation that starts on a window at rest, or
# whose animation ended, starts from where the window was at some time in the step
# (earlier than its first frame when picom held the animations). Where one ends, the two
# meet at its end, with the step of its last frame as allowance (it may still bend
# toward rest in the part not seen). tests/check_frames_test.py runs the styles' motion through this.
#   jumps    the corners, the crop edges where they cut the window, and the opacity (half
#            of one 8-bit level instead of half a pixel) are continuous
#   pops     a window appears or disappears only where nothing of it shows: opacity
#            times the area it shows, in pixels, worked out from its pose as the frames
#            on either side predict it (also those where it is turned away), reaches 0
#            within the step (with the same allowance, in pixels)
#   drawn    picom draws each window within 1 px, each way, of where its script puts it
#            (it rounds the window's place to whole pixels)
#   stops    a window that comes to rest at the end of its animation (or holds there)
#            arrives without speed: the speed its last frames predict for the end moves
#            it less than half a pixel per frame (plus the prediction's error), or its
#            last frames show it slowing (where frames are few, a smooth stop can take
#            less than a frame; a stop at speed doesn't slow before it)
#   rest     at the end, every window is drawn flat, and no animation runs
#   seams    (with --seams) the workspaces of a monitor tile it with no gap or overlap
#            wider than half a pixel: each workspace is the monitor moved and scaled the
#            way its windows are, cut to their crop; for styles whose model tiles (slide,
#            push, wipe, fold), and only in frames of a switch (scripts with var-slide)
# and for the styles that turn a prism (their scripts have var-sides):
#   front    in the styles that move the prism back for it (their scripts have
#            var-fit), the windows of the side nearest to the front stay on their
#            monitor, within half a pixel; not checked while var-fit is below 1, which is
#            a choice to let the side leave the monitor
#   lines    a turned window is a flat rectangle seen from the middle of its monitor at
#            var-d: from each edge across the turn and the window's size, the depth of
#            that edge follows; the edges across the turn are level, the window keeps its
#            size along the turn and across it, and the windows of one workspace lie in
#            one plane, each within half a pixel
#   faces    a window is not drawn (H) exactly when its side faces away from the eye
#            (var-pan, if the script has it, moves the prism along the turn)
#   prism    in each frame, the windows of a switch on one monitor turn on the same
#            prism: the same var-sides, var-camera, var-fit, var-sideways, var-length
# A prism check that finds nothing to check fails: the script may have renamed the
# variables it reads (var-pos, var-sides, var-sideways, var-d, var-back, var-r, var-cos,
# var-sin).
#
# Information, not failures:
#   held     frames in which picom didn't advance an animation (the window stood still)
#   kinks    changes of speed above half a pixel per frame: where an animation starts on
#            a moving window (an interruption: how it goes on is the style's choice),
#            where it starts from rest (how a curve starts is the style's choice too),
#            and inside an animation (where two parts of a script meet, like a max)
#
# Usage: check-frames.py LOG --monitor X,Y,W,H [--monitor ...] [--from T] [--seams]
#        [--plot PNG] [--strip PNG] [--strip-frames N] [--strip-length S]
# --plot draws, over time, var-pos (if the script has it), the share of each window
# that shows, and the time between frames. --strip draws N frames, evenly spread over S
# seconds, as outlines: what picom drew, without Xvfb in between (needs matplotlib).
# --from T leaves out the frames before T (CLOCK_MONOTONIC seconds, as Python's
# time.monotonic()), for the checks, the plot and the strip; times in the report count
# from the first frame left. Without it, the strip starts at the second with the most
# frames of a running animation. Exits 1 if a check fails.

import argparse
import copy
import math
import sys
from collections import defaultdict

# What a screen can't show: half a pixel, and half of one step of an 8-bit channel.
HALF_PX = 0.5
HALF_LEVEL = 0.5 / 255
# The log writes corners and crops with 2 decimals and opacity with 4. A prediction
# through three frames, half a step or a step out, weighs them by at most 7 in all.
ROUND_PX = 7 * 0.005
ROUND_OPACITY = 7 * 0.00005
# Times tried within a step for where the two predictions meet, before the exact search.
SAMPLES = 9


class Layer:
    def __init__(self, p):
        self.window = p[2]
        self.state = p[3]
        # The window's place without animation.
        self.box = [int(x) for x in p[4:8]]
        c = [float(x) for x in p[8:16]]
        # Mesh order: top left, top right, bottom left, bottom right.
        self.corners = [(c[0], c[1]), (c[2], c[3]), (c[4], c[5]), (c[6], c[7])]
        self.opacity = float(p[16])
        self.crop = [float(x) for x in p[17:21]]
        self.script = None
        self.started = False
        # The script of the animation this one interrupted, as it handed over (X record).
        self.handover = None

    def with_script(self, script):
        """This layer with another script, such as the one in its handover."""
        other = copy.copy(self)
        other.script = script
        return other

    def box_corners(self):
        x, y, w, h = self.box
        return [(x, y), (x + w, y), (x, y + h), (x + w, y + h)]

    def pose(self):
        """The corners and crop box the script asks for, before picom rounds the window's
        place and crop to whole pixels; the drawn ones for a window without a script."""
        s = self.script
        if s is None:
            return self.corners, crop_box(self)
        x, y, w, h = self.box
        x += s.get("offset-x", 0)
        y += s.get("offset-y", 0)
        w *= s.get("scale-x", 1)
        h *= s.get("scale-y", 1)
        corners = []
        for i, name in enumerate(("top-left", "top-right", "bottom-left", "bottom-right")):
            corners.append((x + (i % 2) * w + s.get(f"corner-{name}-x", 0),
                            y + (i // 2) * h + s.get(f"corner-{name}-y", 0)))
        if all(f"crop-{k}" in s for k in ("x", "y", "width", "height")):
            box = [s["crop-x"], s["crop-y"], s["crop-x"] + s["crop-width"],
                   s["crop-y"] + s["crop-height"]]
        else:
            box = crop_box(self)
        return corners, box


def parse(path):
    starts, frames, animations = {}, defaultdict(dict), []
    pending = []
    started = defaultdict(set)
    handovers, pending_handovers = defaultdict(dict), {}
    with open(path) as f:
        if not f.readline().startswith("# picom frame timing log"):
            sys.exit(f"{path}: not a picom frame timing log")
        for number, line in enumerate(f, start=2):
            if not line.endswith("\n"):
                break
            p = line.rstrip("\n").split(",")
            if (p[0] == "W" and len(p) != 21) or (p[0] == "E" and len(p) < 5):
                sys.exit(f"{path}:{number}: {p[0]} record with {len(p)} fields; does this "
                         "picom write another frame layout format?")
            if p[0] == "R":
                n = int(p[1])
                starts[n] = int(p[3])
                frames[n]
                # An A record's first pose is in the frame whose R record comes next, and
                # so is the pose that follows an X record.
                started[n].update(pending)
                pending = []
                handovers[n].update(pending_handovers)
                pending_handovers = {}
            elif p[0] == "W":
                frames[int(p[1])][p[2]] = Layer(p)
            elif p[0] == "E":
                layer = frames[int(p[1])].get(p[2])
                if layer:
                    layer.script = {"elapsed": float(p[3]), "duration": float(p[4])}
                    for kv in p[5:]:
                        k, v = kv.split("=", 1)
                        layer.script[k] = float(v)
            elif p[0] == "A":
                animations.append((int(p[1]) / 1e6, p[2], p[3]))
                pending.append(p[2])
            elif p[0] == "X":
                script = {"elapsed": float(p[3]), "duration": float(p[4])}
                for kv in p[5:]:
                    k, v = kv.split("=", 1)
                    script[k] = float(v)
                pending_handovers[p[2]] = script
    # W records come before the frame's R record; a frame cut off at the end has none.
    frames = {n: f for n, f in frames.items() if n in starts}
    if not any(frames.values()):
        sys.exit(f"{path}: no layout records; was frame_layout=1 set?")
    for n, layers in frames.items():
        for w in started.get(n, ()):
            if w in layers:
                layers[w].started = True
        for w, script in handovers.get(n, {}).items():
            if w in layers:
                layers[w].handover = script
    return starts, frames, animations


def around(c):
    return [c[0], c[1], c[3], c[2]]


def area(poly):
    return abs(sum(poly[i][0] * poly[i - 1][1] - poly[i - 1][0] * poly[i][1]
                   for i in range(len(poly)))) / 2


def signed_area(poly):
    return sum(poly[i][0] * poly[i - 1][1] - poly[i - 1][0] * poly[i][1]
               for i in range(len(poly))) / 2


def perimeter(poly):
    return sum(math.dist(poly[i], poly[i - 1]) for i in range(len(poly)))


def clip(poly, box):
    """Sutherland-Hodgman: the part of convex `poly` inside box (x0, y0, x1, y1)."""
    x0, y0, x1, y1 = box
    for inside, cut in (
            (lambda p: p[0] >= x0, lambda a, b: (x0, a[1] + (b[1] - a[1]) * (x0 - a[0]) / (b[0] - a[0]))),
            (lambda p: p[0] <= x1, lambda a, b: (x1, a[1] + (b[1] - a[1]) * (x1 - a[0]) / (b[0] - a[0]))),
            (lambda p: p[1] >= y0, lambda a, b: (a[0] + (b[0] - a[0]) * (y0 - a[1]) / (b[1] - a[1]), y0)),
            (lambda p: p[1] <= y1, lambda a, b: (a[0] + (b[0] - a[0]) * (y1 - a[1]) / (b[1] - a[1]), y1))):
        out = []
        for i, b in enumerate(poly):
            a = poly[i - 1]
            if inside(b):
                if not inside(a):
                    out.append(cut(a, b))
                out.append(b)
            elif inside(a):
                out.append(cut(a, b))
        poly = out
        if not poly:
            break
    return poly


def flat(layer):
    (ax, ay), (bx, by), (cx, cy), (dx, dy) = layer.pose()[0]
    return abs(ax - cx) < 0.5 and abs(bx - dx) < 0.5 and abs(ay - by) < 0.5 and abs(cy - dy) < 0.5


def at_rest(layer):
    x, y, w, h = layer.box
    rest = [(x, y), (x + w, y), (x, y + h), (x + w, y + h)]
    return all(math.dist(p, q) < 0.5 for p, q in zip(layer.corners, rest))


def crop_box(layer):
    x, y, w, h = layer.crop
    if math.isfinite(w) and math.isfinite(h):
        return [x, y, x + w, y + h]
    return [-math.inf, -math.inf, math.inf, math.inf]


def cutting_edges(layer):
    """Which crop edges (left, top, right, bottom) cut the window: lie inside its bounding
    box. Only those change what shows."""
    corners, box = layer.pose()
    xs = [c[0] for c in corners]
    ys = [c[1] for c in corners]
    lo = (min(xs), min(ys), min(xs), min(ys))
    hi = (max(xs), max(ys), max(xs), max(ys))
    return {k for k in range(4) if lo[k] < box[k] < hi[k]}


def crop_values(layer, edges):
    """The crop edges given; no crop counts as a crop at the window's own edge."""
    corners, box = layer.pose()
    xs = [c[0] for c in corners]
    ys = [c[1] for c in corners]
    own = (min(xs), min(ys), max(xs), max(ys))
    return [box[k] if math.isfinite(box[k]) else own[k] for k in edges]


def newton(ts, rows):
    """Divided differences of rows (one list of values per time) at times ts, nearest
    first: the first k terms are the polynomial through the k nearest frames."""
    coef = [list(r) for r in rows]
    for j in range(1, len(ts)):
        for i in range(len(ts) - 1, j - 1, -1):
            d = ts[i] - ts[i - j]
            coef[i] = [(a - b) / d for a, b in zip(coef[i], coef[i - 1])]
    return coef


class Side:
    """What the frames on one side of a step predict, for a list of values per frame.
    frames: (time, values), nearest to the step first; static: the values can't change
    (no animation, or one that holds its pose). slack: what the prediction may miss by
    besides that (see check_step).

    The prediction is a range: from the straight line through the two nearest frames to
    the curve through three, widened by the next term (from a fourth frame): the bend
    the frames show keeps up, or eases off. With fewer frames, the last term counts
    twice, which overestimates."""

    def __init__(self, frames, static, slack=0.0):
        self.static = static
        self.slack = slack
        self.step = 0.0
        self.frames = frames
        self.ts = [t for t, _ in frames]
        self.coef = newton(self.ts, [v for _, v in frames])
        m = len(frames)
        self.size = len(frames[0][1])
        self.k = 1 if static else min(3, m)
        self.err_term = None if static else (self.k if m > self.k else self.k - 1)
        self.unknown = not static and m < 2

    def at(self, t):
        """The predicted values and speeds at time t, and how far each may be off."""
        n = self.size
        value, speed = [0.0] * n, [0.0] * n
        err, derr = [0.0] * n, [0.0] * n
        prod, dprod = 1.0, 0.0
        for i in range(max(self.k, (self.err_term or 0) + 1)):
            if i >= len(self.coef):
                break
            c = self.coef[i]
            if i < self.k:
                for q in range(n):
                    value[q] += c[q] * prod
                    speed[q] += c[q] * dprod
            # The last term of the prediction (the bend), and the next one.
            if i > 0 and (i == self.k - 1 or i == self.err_term):
                for q in range(n):
                    err[q] += abs(c[q] * prod)
                    derr[q] += abs(c[q] * dprod)
            dprod = dprod * (t - self.ts[i]) + prod
            prod *= t - self.ts[i]
        return value, speed, err, derr


def slowing(side):
    """Whether the window moved slower in its last frame than in the one before, by more
    than the log's rounding can show (corner values only)."""
    if len(side.frames) < 3:
        return False
    (t0, p0), (t1, p1), (t2, p2) = side.frames[:3]
    last = corner_steps(p0, p1) / (t0 - t1)
    before = corner_steps(p1, p2) / (t1 - t2)
    return (before - last) * (t0 - t1) > ROUND_PX


def point(t, values):
    """A side that is one known pose."""
    return Side([(t, values)], True)


def meet(left, right, t0, t1, distance):
    """How far apart the two predictions are where they come closest within [t0, t1],
    beyond what they may be off by. distance(a, b, ea, eb) gives (apart, allowed) for two
    value lists. Returns (excess, time, apart, allowed)."""
    def at(t):
        a, _, ea, _ = left.at(t)
        b, _, eb, _ = right.at(t)
        apart, allowed = distance(a, b, ea, eb)
        allowed += left.slack + right.slack
        return apart - allowed, t, apart, allowed

    times = [t0 + (t1 - t0) * i / (SAMPLES - 1) for i in range(SAMPLES)]
    best = min((at(t) for t in times), key=lambda r: r[0])
    if best[0] <= 0:
        return best
    # The two may cross between samples: find where each value's difference changes
    # sign and look there.
    diffs = [[a - b for a, b in zip(left.at(t)[0], right.at(t)[0])] for t in times]
    for q in range(left.size):
        for i in range(SAMPLES - 1):
            lo, hi = times[i], times[i + 1]
            dlo, dhi = diffs[i][q], diffs[i + 1][q]
            if (dlo > 0) == (dhi > 0):
                continue
            for _ in range(30):
                mid = (lo + hi) / 2
                dmid = left.at(mid)[0][q] - right.at(mid)[0][q]
                if (dmid > 0) == (dlo > 0):
                    lo, dlo = mid, dmid
                else:
                    hi = mid
            r = at((lo + hi) / 2)
            if r[0] < best[0]:
                best = r
    return best


def bent_step(left, right, t0, t1, p0, p1, floor):
    """Whether a step within one animation, from p0 at t0 to p1 at t1, is one that a
    continuous motion makes whose speed bends within the step, from the speed the frames
    before show to the speed the frames after show, at its own time for each value: a
    bend of the script followed by more bending, which the two sides' curves don't
    predict. Each value moves by what the slowest and the fastest of those speeds (each
    side's at both ends of the step, with their errors) allow in the step, plus floor."""
    dt = t1 - t0
    lo, hi = [math.inf] * left.size, [-math.inf] * left.size
    for side in (left, right):
        for t in (t0, t1):
            _, v, _, dv = side.at(t)
            for q in range(left.size):
                lo[q] = min(lo[q], v[q] - dv[q])
                hi[q] = max(hi[q], v[q] + dv[q])
    return all(lo[q] * dt - floor <= p1[q] - p0[q] <= hi[q] * dt + floor
               for q in range(left.size))


def smooth_step(left, right, t0, t1, p0, p1, distance, floor):
    """Whether a step within one animation, from p0 at t0 to p1 at t1, is continuous:
    the two sides meet, one end of the step lies on the other side's piece, or the step
    is a bent one (see bent_step). The second is for a bend in the step next to it,
    which the frames of one side straddle; that step is checked on its own. A jump fails
    all of them. Returns meet()'s result for the two sides, with its excess set to 0
    when one of the others holds."""
    best = meet(left, right, t0, t1, distance)
    if best[0] > 0 and (at_time(point(t0, p0), right, t0, distance)[0] <= 0 or
                        at_time(left, point(t1, p1), t1, distance)[0] <= 0 or
                        bent_step(left, right, t0, t1, p0, p1, floor)):
        best = (0.0,) + best[1:]
    return best


def at_time(left, right, t, distance):
    """meet() at a known time t."""
    a, _, ea, _ = left.at(t)
    b, _, eb, _ = right.at(t)
    apart, allowed = distance(a, b, ea, eb)
    allowed += left.slack + right.slack
    return apart - allowed, t, apart, allowed


def corner_steps(a, b):
    return max(math.hypot(a[c] - b[c], a[c + 1] - b[c + 1]) for c in range(0, len(a) - 1, 2))


def scalar_steps(a, b):
    return max(abs(x - y) for x, y in zip(a, b))


def corner_distance(a, b, ea, eb):
    apart = allowed = 0.0
    worst = -math.inf
    for c in range(0, 8, 2):
        d = math.hypot(a[c] - b[c], a[c + 1] - b[c + 1])
        e = math.hypot(ea[c], ea[c + 1]) + math.hypot(eb[c], eb[c + 1])
        if d - e > worst:
            worst, apart, allowed = d - e, d, e
    return apart, allowed + HALF_PX + ROUND_PX


def scalar_distance(floor):
    def distance(a, b, ea, eb):
        worst, apart, allowed = -math.inf, 0.0, 0.0
        for q in range(len(a)):
            d, e = abs(a[q] - b[q]), ea[q] + eb[q]
            if d - e > worst:
                worst, apart, allowed = d - e, d, e
        return apart, allowed + floor
    return distance


class Checker:
    def __init__(self, starts, frames, animations, monitors, start=None, seams=False):
        self.frames, self.animations = frames, animations
        self.starts_us = starts
        # picom's animation clock: the frame's start, rounded down to the millisecond.
        self.starts = {n: (us // 1000) / 1000 for n, us in starts.items()}
        self.monitors = monitors
        self.seams = seams
        self.order = [n for n in sorted(frames) if start is None or starts[n] / 1e6 >= start]
        if not self.order:
            sys.exit(f"no frames from {start}")
        self.start = self.starts[self.order[0]]
        self.failures = defaultdict(list)
        self.notes = {}
        self.counts = {}
        self.kinks = defaultdict(list)
        self.segments = self.find_segments()

    def find_segments(self):
        """For each window and frame: an id that changes where an animation starts or
        ends, and whether the window's pose is static there. Also the window's time: in
        an animation, when it started plus its elapsed time, which stands still while
        picom holds the animations (while the screen pauses); otherwise the frame's."""
        seg = {}
        self.times = {}
        self.held = []
        for w in {w for n in self.order for w in self.frames[n]}:
            ident, prev, origin = 0, None, 0.0
            for n in self.order:
                layer = self.frames[n].get(w)
                if layer is None:
                    prev = None
                    continue
                s = layer.script
                kind = "rest" if s is None else (
                    "held" if s["elapsed"] >= s["duration"] else "anim")
                new = (prev is None or layer.started or kind != prev[0] or
                       (kind == "anim" and s["elapsed"] < prev[1]) or
                       (kind == "rest" and layer.box != prev[2]))
                if new:
                    ident += 1
                    origin = self.starts[n] - (s["elapsed"] if kind == "anim" else 0.0)
                elif kind == "anim":
                    lost = (self.starts[n] - prev[3]) - (s["elapsed"] - prev[1])
                    if lost > 0.0015:
                        self.held.append((lost, n, w))
                seg[(n, w)] = (ident, kind != "anim")
                self.times[(n, w)] = origin + s["elapsed"] if kind == "anim" else self.starts[n]
                prev = (kind, s["elapsed"] if s else 0, layer.box, self.starts[n])
        return seg

    def monitor_of(self, layer):
        for m in self.monitors:
            if layer.crop[:2] == [m[0], m[1]] and layer.crop[2:] == [m[2], m[3]]:
                return m
        xs = [c[0] for c in layer.corners]
        ys = [c[1] for c in layer.corners]
        cx, cy = sum(xs) / 4, sum(ys) / 4
        for m in self.monitors:
            if m[0] <= cx < m[0] + m[2] and m[1] <= cy < m[1] + m[3]:
                return m
        return None

    def visible(self, layer):
        """The part of the window that shows: inside its crop box and its monitor."""
        corners, box = layer.pose()
        m = self.monitor_of(layer)
        if m:
            box = [max(box[0], m[0]), max(box[1], m[1]),
                   min(box[2], m[0] + m[2]), min(box[3], m[1] + m[3])]
        if box[0] >= box[2] or box[1] >= box[3]:
            return []
        return clip(around(corners), box)

    def shown_px(self, layer):
        """How much of the window shows, in pixels: opacity times the area it shows."""
        if layer is None or layer.state != "D":
            return 0.0
        return layer.opacity * area(self.visible(layer))

    def shown(self, layer):
        """The share of the window that shows, for the plot."""
        if layer is None or layer.state != "D":
            return 0.0
        return self.shown_px(layer) / max(layer.box[2] * layer.box[3], 1)

    def fail(self, check, n, w, text):
        layer = self.frames[n].get(w)
        pos = ""
        if layer and layer.script and "var-pos" in layer.script:
            pos = f", var-pos {layer.script['var-pos']:.2f}"
        self.failures[check].append(
            f"frame {n} ({self.starts[n] - self.start:.3f} s), window {w}{pos}: {text}")

    def side(self, w, i, step, values, drawn, steps=scalar_steps):
        """The frames of window w from order[i] on, going by step (-1 or 1), in the same
        animation, as a Side over values(layer); steps(a, b) is how far the values move
        from one frame to the next, which sets the side's step: the most it moved in
        one frame."""
        n = self.order[i]
        ident, static = self.segments[(n, w)]
        frames = []
        while 0 <= i < len(self.order) and len(frames) < 4:
            n = self.order[i]
            layer = self.frames[n].get(w)
            if layer is None or not drawn(layer) or self.segments[(n, w)][0] != ident:
                break
            # Two frames at the same time show the same pose.
            t = self.times[(n, w)]
            if not frames or abs(t - frames[-1][0]) > 1e-9:
                frames.append((t, values(layer)))
            elif layer.state == "D":
                frames[-1] = (t, values(layer))
            if static:
                break
            i += step
        side = Side(frames, static)
        side.step = 0.0
        if not static and len(frames) > 1:
            side.step = max(steps(a, b) for (_, a), (_, b) in zip(frames, frames[1:]))
        return side

    # --- continuity ---

    def check_steps(self):
        counts = defaultdict(int)
        for check in ("jumps", "pops", "stops", "drawn"):
            self.notes[check] = ""
        drawn = lambda layer: layer.state == "D"
        worst = 0.0
        for n in self.order:
            for w, layer in self.frames[n].items():
                if layer.state != "D" or layer.script is None:
                    continue
                counts["drawn"] += 1
                off = max(max(abs(p[0] - q[0]), abs(p[1] - q[1]))
                          for p, q in zip(layer.corners, layer.pose()[0]))
                worst = max(worst, off)
                # picom rounds the window's place to whole pixels: less than 1 px each way.
                if off >= 1 + ROUND_PX:
                    self.fail("drawn", n, w, f"drawn {off:.2f} px from where its script puts it")
        self.notes["drawn"] = (f"{counts['drawn']} animated windows drawn within "
                               f"{worst:.2f} px of where their scripts put them")
        for i in range(1, len(self.order)):
            n0, n1 = self.order[i - 1], self.order[i]
            t0, t1 = self.starts[n0], self.starts[n1]
            # Before picom draws its first window, the windows already on screen are not
            # its to show: they come up at once when it starts.
            if not self.frames[n0]:
                continue
            for w in set(self.frames[n0]) | set(self.frames[n1]):
                a, b = self.frames[n0].get(w), self.frames[n1].get(w)
                da, db = a is not None and drawn(a), b is not None and drawn(b)
                if da != db:
                    counts["pops"] += 1
                    self.check_pop(w, i, da)
                    continue
                if not da:
                    continue
                sa, sb = self.segments[(n0, w)], self.segments[(n1, w)]
                # A window that the window manager moves while no animation runs is not
                # the style's doing.
                if sa[1] and sb[1] and (sa == sb or a.script is None and b.script is None):
                    continue
                counts["jumps"] += 1
                self.check_step(w, i, t0, t1)
        self.notes["jumps"] = f"{counts['jumps']} steps of drawn windows checked"
        self.notes["pops"] = f"{counts['pops']} windows that appear or disappear"
        if not self.notes["stops"]:
            self.notes["stops"] = "no animation ended on a drawn window"

    def check_step(self, w, i, t0, t1):
        n0, n1 = self.order[i - 1], self.order[i]
        drawn = lambda layer: layer.state == "D"
        groups = (
            ("corners", lambda l: [v for c in l.pose()[0] for v in c], corner_distance, "px",
             corner_steps, HALF_PX + ROUND_PX),
            ("crop", lambda l: crop_values(l, cut), scalar_distance(HALF_PX + ROUND_PX), "px",
             scalar_steps, HALF_PX + ROUND_PX),
            ("opacity", lambda l: [l.opacity], scalar_distance(HALF_LEVEL + ROUND_OPACITY), "",
             scalar_steps, HALF_LEVEL + ROUND_OPACITY),
        )
        cut = sorted(cutting_edges(self.frames[n0][w]) | cutting_edges(self.frames[n1][w]))
        kink_done = False
        sides = {}
        for name, values, distance, unit, steps, floor in groups:
            if name == "crop" and not cut:
                continue
            sides[name] = (self.side(w, i - 1, -1, values, drawn, steps),
                           self.side(w, i, 1, values, drawn, steps))
        # The most the window's corners or crop edges moved in a frame, before the step.
        pixel_step = max((l.step for name, (l, r) in sides.items() if name != "opacity"),
                         default=0.0)
        for name, values, distance, unit, steps, floor in groups:
            if name not in sides:
                continue
            left, right = sides[name]
            if left.unknown or right.unknown:
                continue
            # Each side's own time (see find_segments) at its frame next to the step.
            ta, tb = self.times[(n0, w)], self.times[(n1, w)]
            end = self.end_time(w, i, left, right)
            if end is not None:
                # The animation ends within the step: there. Its script may bend in the
                # part of the step that isn't seen, but only toward rest: by at most what
                # it moved in a frame.
                left.slack = left.step
                excess, t, apart, allowed = at_time(left, right, end, distance)
                t_right = t
            elif self.segments[(n0, w)] != self.segments[(n1, w)] and self.handed(n0, n1, w):
                # A new animation took over from a running one, which picom logged as
                # it handed over (an X record): the new one starts from exactly that
                # pose, at its elapsed 0. The old one isn't seen from its last frame to
                # then, and its script may bend there (a max that switches): it may
                # turn back, by up to twice the step its frames make, and a bend can
                # start a crop edge that stood still (as push's does when var-pos
                # crosses 0), at the speed of any of the window's corners and crop
                # edges.
                old, new = self.frames[n0][w], self.frames[n1][w]
                handed = values(new.with_script(new.handover))
                tx = ta + new.handover["elapsed"] - old.script["elapsed"]
                left.slack = 2 * (pixel_step if name == "crop" else left.step)
                excess, t, apart, allowed = at_time(left, point(tx, handed), tx, distance)
                t_right = tb
                if excess <= 0:
                    start = tb - new.script["elapsed"]
                    excess, t, apart, allowed = at_time(point(start, handed), right, start,
                                                        distance)
                    if excess > 0:
                        what = "the corners" if name == "corners" else f"the {name}"
                        self.fail("jumps", n1, w, f"{what} jump where the new animation "
                                  f"takes over: it starts {apart:.3g}{unit} from where "
                                  f"the old one handed over, {allowed:.3g}{unit} allowed")
                        continue
            elif self.segments[(n0, w)] != self.segments[(n1, w)]:
                # A new animation starts on a window at rest, or one whose animation
                # ended: from where it was at some time in the step, at its first frame,
                # or earlier if picom held the animations (while the screen pauses).
                left.slack = 2 * max(left.step, right.step)
                first = Side([(ta + t1 - t0, right.at(tb)[0])], True)
                excess, t, apart, allowed = meet(left, first, ta, ta + t1 - t0, distance)
                t_right = tb
                if name == "corners" and excess <= 0:
                    self.note_lost(w, n1, left, t, ta + t1 - t0 - t)
            else:
                p0 = values(self.frames[n0][w])
                p1 = values(self.frames[n1][w])
                excess, t, apart, allowed = smooth_step(left, right, ta, tb, p0, p1, distance,
                                                        floor)
                t_right = t
            if excess > 0:
                what = "the corners" if name == "corners" else f"the {name}"
                self.fail("jumps", n1, w, f"{what} jump: the frames before and after meet "
                          f"{apart:.3g}{unit} apart, {allowed:.3g}{unit} allowed")
                continue
            if name == "corners" and not kink_done:
                kink_done = True
                gaps = [t1 - t0] + [abs(x.ts[1] - x.ts[0]) for x in (left, right)
                                    if len(x.ts) > 1]
                self.note_kink(w, n1, left, t, right, t_right, min(gaps))

    def handed(self, n0, n1, w):
        """Whether a running animation of window w handed over to a new one in frame n1,
        and picom logged that (an X record)."""
        return self.frames[n1][w].handover is not None and self.frames[n0][w].script is not None

    def end_time(self, w, i, left, right):
        """Where an animation on window w ends within step i, on its own time, if one
        does there: its duration after it started, which the frame before says."""
        n0, n1 = self.order[i - 1], self.order[i]
        a = self.frames[n0].get(w)
        if right.static and not left.static and not self.frames[n1][w].started and a.script:
            left_over = max(0.0, a.script["duration"] - a.script["elapsed"])
            return self.times[(n0, w)] + min(left_over, self.starts[n1] - self.starts[n0])
        return None

    def note_lost(self, w, n1, left, t, lost):
        """A new animation that starts from where the old one was before its first
        frame: the window stood still for that time."""
        _, v, _, _ = left.at(t)
        moved = max(math.hypot(v[c], v[c + 1]) for c in range(0, 8, 2)) * lost
        if moved > HALF_PX + ROUND_PX and not left.static:
            self.held.append((lost, n1, w))

    def note_kink(self, w, n1, left, ta, right, tb, dt):
        """The change of speed where the frames before (at their time ta) and after (at
        tb) meet, in pixels per frame of dt."""
        _, va, _, ea = left.at(ta)
        _, vb, _, eb = right.at(tb)
        change = err = 0.0
        for c in range(0, 8, 2):
            d = math.hypot(va[c] - vb[c], va[c + 1] - vb[c + 1]) * dt
            if d > change:
                change = d
                err = (math.hypot(ea[c], ea[c + 1]) + math.hypot(eb[c], eb[c + 1])) * dt
        allowed = HALF_PX + ROUND_PX + err
        moving_before = not left.static and max(abs(v) for v in va) * dt > HALF_PX
        if self.frames[n1][w].started:
            kind = "at an interruption" if moving_before else "at a start from rest"
        elif right.static and not left.static:
            self.counts["stops"] = self.counts.get("stops", 0) + 1
            self.notes["stops"] = f"{self.counts['stops']} animations ended on a drawn window"
            # Where frames are few, a smooth stop can happen within the last frame, which
            # the frames before can't predict; but then they show the window slowing. A
            # stop at speed doesn't slow before it.
            if change > allowed and not slowing(left):
                self.fail("stops", n1, w, f"comes to rest at {change:.3g} px per frame, "
                          f"{allowed:.3g} px allowed, without slowing in its last frames")
            return
        else:
            kind = "inside an animation"
        if change > allowed:
            self.kinks[kind].append((change, n1, w))

    def check_pop(self, w, i, before):
        """Window w is drawn in order[i - 1] (before) or in order[i], not both. From the
        frames on either side that say where it is (drawn or not, as the frame layout
        gives the corners of a window turned away too), predict its corners, crop and
        opacity in the step: at some time there, by one side's prediction, what shows of
        it must be about nothing. The area comes from the predicted pose, not from a
        prediction of the area, which bends where the window crosses its crop."""
        n0, n1 = self.order[i - 1], self.order[i]
        t0, t1 = self.starts[n0], self.starts[n1]
        dt = t1 - t0
        posed = lambda layer: layer.state != "I"
        layer = self.frames[n0 if before else n1][w]
        m = self.monitor_of(layer)
        screen = [m[0], m[1], m[0] + m[2], m[1] + m[3]] if m else [-1e6, -1e6, 1e6, 1e6]
        facing = signed_area(around(layer.box_corners())) >= 0

        def values(l):
            corners, box = l.pose()
            box = [max(a, b) for a, b in zip(box[:2], screen[:2])] + \
                [min(a, b) for a, b in zip(box[2:], screen[2:])]
            return [v for c in corners for v in c] + box + [l.opacity]

        def excess(side, t):
            v, _, e, _ = side.at(t)
            corners = around([(v[0], v[1]), (v[2], v[3]), (v[4], v[5]), (v[6], v[7])])
            opacity = max(0.0, v[12])
            if (signed_area(corners) >= 0) != facing or v[8] >= v[10] or v[9] >= v[11]:
                return -1.0, 0.0, 0.0
            poly = clip(corners, v[8:12])
            shown = opacity * area(poly)
            err = max(e[:12]) + ROUND_PX
            allowed = HALF_PX + opacity * perimeter(poly) * err + area(poly) * (e[12] + ROUND_OPACITY)
            return shown - allowed, shown, allowed

        a, b = self.frames[n0].get(w), self.frames[n1].get(w)
        results = []
        if a is not None and posed(a):
            left = self.side(w, i - 1, -1, values, posed)
            ta = self.times[(n0, w)]
            if left.static:
                times = [ta]
            elif a.script and a.script["elapsed"] + dt >= a.script["duration"]:
                # Its animation ends within this step: there.
                times = [ta + a.script["duration"] - a.script["elapsed"]]
            else:
                times = [ta + dt * k / 64 for k in range(65)]
            if not left.unknown:
                results += [excess(left, t) for t in times]
        if b is not None and posed(b):
            right = self.side(w, i, 1, values, posed)
            tb = self.times[(n1, w)]
            if right.static or b.started or (a is not None and self.segments.get((n0, w)) !=
                                              self.segments[(n1, w)]):
                # An animation that starts here has its first pose here.
                times = [tb]
            else:
                times = [tb - dt + dt * k / 64 for k in range(65)]
            if not right.unknown:
                results += [excess(right, t) for t in times]
        if not results:
            self.counts["pops-unchecked"] = self.counts.get("pops-unchecked", 0) + 1
            return
        worst, shown, allowed = min(results)
        if worst > 0:
            what = "disappears" if before else "appears"
            states = "".join(x.state if x else "-" for x in (a, b))
            self.fail("pops", n1, w, f"{what} at once: {shown:.0f} px of it show where it "
                      f"stops being drawn, {allowed:.0f} px allowed (states {states})")

    def check_rest(self):
        n = self.order[-1]
        last = self.frames[n]
        for w, layer in last.items():
            if layer.script and layer.script["elapsed"] < layer.script["duration"]:
                self.fail("rest", n, w, "its animation still runs in the last frame")
            if layer.state == "D" and not at_rest(layer):
                self.fail("rest", n, w, f"not at {layer.box} in the last frame: {layer.corners}")
        self.notes["rest"] = f"{len(last)} windows in the last frame, {self.order[-1]}"

    # --- seams ---

    def check_seams(self):
        count = 0
        for n in self.order:
            per_monitor = defaultdict(list)
            skip = set()
            for w, layer in self.frames[n].items():
                if layer.state != "D":
                    continue
                m = self.monitor_of(layer)
                if m is None:
                    continue
                key = tuple(m)
                if (layer.script is not None and "var-slide" not in layer.script) or \
                        not flat(layer):
                    skip.add(key)
                elif layer.script is None or layer.script.get("var-slide", 0) >= 0.5:
                    per_monitor[key].append((w, layer))
            for key, layers in per_monitor.items():
                if key in skip:
                    continue
                regions = self.workspace_regions(key, layers)
                if len(regions) < 2:
                    continue
                count += 1
                gap, overlap = self.tiling(key, regions)
                if gap > HALF_PX + ROUND_PX:
                    self.fail("seams", n, layers[0][0], f"a gap {gap:.2f} px wide between "
                              f"the {len(regions)} workspaces on monitor {list(key)}")
                if overlap > HALF_PX + ROUND_PX:
                    self.fail("seams", n, layers[0][0], f"an overlap {overlap:.2f} px wide "
                              f"between the {len(regions)} workspaces on monitor {list(key)}")
        self.notes["seams"] = f"{count} frames with workspaces side by side on a monitor"
        self.counts["seams"] = count

    def workspace_regions(self, m, layers):
        """The monitor, moved and scaled as each group of windows that move together, cut
        to their crop and the monitor: where each workspace is."""
        groups = []
        for w, layer in layers:
            x, y, bw, bh = layer.box
            ((x0, y0), (x1, _), (_, y2), _), crop = layer.pose()
            sx, sy = (x1 - x0) / max(bw, 1), (y2 - y0) / max(bh, 1)
            tx, ty = x0 - sx * x, y0 - sy * y
            t = (sx, tx, sy, ty)
            for g in groups:
                if all(abs(a - b) < tol for a, b, tol in zip(t, g[0], (1e-3, 0.5, 1e-3, 0.5))) \
                        and all(abs(a - b) < 0.5 or a == b for a, b in zip(crop, g[1])):
                    break
            else:
                groups.append((t, crop))
        regions = []
        for (sx, tx, sy, ty), crop in groups:
            r = [sx * m[0] + tx, sy * m[1] + ty, sx * (m[0] + m[2]) + tx, sy * (m[1] + m[3]) + ty]
            r = [max(r[0], crop[0], m[0]), max(r[1], crop[1], m[1]),
                 min(r[2], crop[2], m[0] + m[2]), min(r[3], crop[3], m[1] + m[3])]
            if r[2] - r[0] > HALF_PX and r[3] - r[1] > HALF_PX:
                regions.append(r)
        return regions

    def tiling(self, m, regions):
        """The widest gap and overlap in the monitor between the regions: each cell of
        the grid their edges make is covered by none, one or more."""
        xs = sorted({m[0], m[0] + m[2], *[r[0] for r in regions], *[r[2] for r in regions]})
        ys = sorted({m[1], m[1] + m[3], *[r[1] for r in regions], *[r[3] for r in regions]})
        gap = overlap = 0.0
        for xa, xb in zip(xs, xs[1:]):
            for ya, yb in zip(ys, ys[1:]):
                cx, cy = (xa + xb) / 2, (ya + yb) / 2
                if not (m[0] <= cx <= m[0] + m[2] and m[1] <= cy <= m[1] + m[3]):
                    continue
                cover = sum(1 for r in regions if r[0] <= cx <= r[2] and r[1] <= cy <= r[3])
                width = min(xb - xa, yb - ya)
                if cover == 0:
                    gap = max(gap, width)
                elif cover > 1:
                    overlap = max(overlap, width)
        return gap, overlap

    # --- prisms ---

    def prism_layers(self):
        for n in self.order:
            for w, layer in self.frames[n].items():
                s = layer.script
                if s and "var-sides" in s and "var-pos" in s:
                    yield n, w, layer, s

    def check_front(self):
        count = 0
        for n, w, layer, s in self.prism_layers():
            m = self.monitor_of(layer)
            if abs(s["var-pos"]) > 0.5 or layer.state != "D" or m is None or "var-fit" not in s:
                continue
            if s["var-fit"] < 1:
                self.counts["front-chosen"] = self.counts.get("front-chosen", 0) + 1
                continue
            count += 1
            corners = layer.pose()[0]
            xs = [c[0] for c in corners]
            ys = [c[1] for c in corners]
            out = max(m[0] - min(xs), max(xs) - m[0] - m[2], m[1] - min(ys), max(ys) - m[1] - m[3])
            if out > HALF_PX + ROUND_PX:
                self.fail("front", n, w, f"{out:.2f} px off its monitor")
        chosen = self.counts.get("front-chosen", 0)
        self.notes["front"] = f"{count} drawn windows of the side in front" + (
            f", {chosen} not checked: var-fit is below 1" if chosen else "")
        self.counts["front"] = count + chosen

    def edges(self, layer, s, m):
        """The window's two edges across the turn, as (u along the turn on screen, depth,
        scale, across offsets on screen, across offsets of the window) from the middle of
        the monitor, with the depth worked out from the window's size across."""
        sideways = s.get("var-sideways", 0) >= 0.5
        mid_x, mid_y = m[0] + m[2] / 2, m[1] + m[3] / 2
        x, y, bw, bh = layer.box
        c = layer.pose()[0]
        if sideways:
            # Turning about an upright axis: u is x; the edges across are left and right.
            pairs = ((c[0], c[2]), (c[1], c[3]))
            v_true = (y - mid_y, y + bh - mid_y)
            to_uv = lambda p: (p[0] - mid_x, p[1] - mid_y)
        else:
            pairs = ((c[0], c[1]), (c[2], c[3]))
            v_true = (x - mid_x, x + bw - mid_x)
            to_uv = lambda p: (p[1] - mid_y, p[0] - mid_x)
        d = s["var-d"]
        out = []
        for a, b in pairs:
            (ua, va), (ub, vb) = to_uv(a), to_uv(b)
            k = (vb - va) / (v_true[1] - v_true[0])
            if k <= 0:
                return None
            out.append({"u": (ua + ub) / 2, "level": abs(ua - ub), "k": k, "z": d / k - d,
                        "v": (va, vb), "v_true": v_true})
        along = (bw if sideways else bh)
        return out, along, d

    def check_lines(self):
        count = 0
        planes = defaultdict(list)
        for n, w, layer, s in self.prism_layers():
            m = self.monitor_of(layer)
            if layer.state != "D" or m is None or flat(layer) or "var-d" not in s:
                continue
            got = self.edges(layer, s, m)
            if got is None:
                self.fail("lines", n, w, "an edge across the turn is drawn reversed")
                continue
            (e0, e1), along, d = got
            count += 1
            level = max(e0["level"], e1["level"])
            if level > HALF_PX + ROUND_PX:
                self.fail("lines", n, w, f"an edge across the turn is {level:.2f} px off level")
                continue
            across = max(max(abs(v - t * e["k"]) for v, t in zip(e["v"], e["v_true"]))
                         for e in (e0, e1))
            if across > HALF_PX + ROUND_PX:
                self.fail("lines", n, w, f"a corner is {across:.2f} px off where the eye at the "
                          "middle of the monitor sees it across the turn")
                continue
            # The edges in 3D: along-turn place u / k at depth z.
            p0 = (e0["u"] / e0["k"], e0["z"])
            p1 = (e1["u"] / e1["k"], e1["z"])
            length = math.dist(p0, p1)
            if length > 0:
                # Where the second edge is with the window's true size, seen on screen.
                f = along / length
                q = (p0[0] + (p1[0] - p0[0]) * f, p0[1] + (p1[1] - p0[1]) * f)
                kq = d / (d + q[1])
                off = max(abs(q[0] * kq - e1["u"]),
                          max(abs(t * kq - v) for v, t in zip(e1["v"], e1["v_true"])))
                if off > HALF_PX + ROUND_PX:
                    self.fail("lines", n, w, f"the window is drawn {length - along:+.1f} px "
                              f"longer along the turn than it is, {off:.2f} px on screen")
                    continue
                planes[(n, tuple(m), round(s["var-pos"], 6))].append((w, p0, p1, e0, e1, d))
        self.notes["lines"] = f"{count} turned windows"
        self.counts["lines"] = count
        for (n, _, _), group in planes.items():
            if len(group) < 2:
                continue
            ref = max(group, key=lambda g: math.dist(g[1], g[2]))
            (u0, z0), (u1, z1) = ref[1], ref[2]
            du, dz = u1 - u0, z1 - z0
            for w, _, _, e0, e1, d in group:
                if w == ref[0]:
                    continue
                for e in (e0, e1):
                    # The eye's ray through this edge, u (d + z) / d along at depth z, meets
                    # the reference plane, (u0, z0) + l (du, dz), at l.
                    u = e["u"]
                    denom = du - u * dz / d
                    if abs(denom) < 1e-9:
                        continue
                    z = z0 + dz * (u * (d + z0) / d - u0) / denom
                    k = d / (d + z)
                    off = max(abs(t * k - v) for v, t in zip(e["v"], e["v_true"]))
                    if off > HALF_PX + ROUND_PX:
                        self.fail("lines", n, w, f"not in the plane of window {ref[0]} of its "
                                  f"workspace: {off:.2f} px off on screen")
                        break

    def check_faces(self):
        count = 0
        need = ("var-d", "var-back", "var-r", "var-cos", "var-sin")
        for n, w, layer, s in self.prism_layers():
            if not all(k in s for k in need):
                self.fail("faces", n, w, f"the script lacks one of {', '.join(need)}")
                break
            if layer.state not in "DH":
                continue
            # The eye is var-d in front of the monitor; the side, var-r from the axis of
            # a prism moved var-pan along the turn, faces it if
            # (d + back + r) cos a > r + pan sin a.
            facing = ((s["var-d"] + s["var-back"] + s["var-r"]) * s["var-cos"] - s["var-r"] -
                      s.get("var-pan", 0) * s["var-sin"])
            if abs(facing) < 0.005 * s["var-d"]:
                continue
            count += 1
            if (facing > 0) != (layer.state == "D"):
                self.fail("faces", n, w, "not drawn, but faces the eye" if facing > 0
                          else "drawn, but faces away")
        self.notes["faces"] = f"{count} windows not edge on"
        self.counts["faces"] = count

    def check_prism(self):
        count = 0
        keys = ("var-sides", "var-camera", "var-fit", "var-sideways", "var-length")
        for n in self.order:
            sets = defaultdict(dict)
            for w, layer in self.frames[n].items():
                s = layer.script
                m = self.monitor_of(layer)
                if s and "var-sides" in s and s.get("var-slide", 0) >= 0.5 and m:
                    sets[tuple(m)][w] = tuple(s.get(k) for k in keys)
            for windows in sets.values():
                count += 1
                if len(set(windows.values())) > 1:
                    w = next(iter(windows))
                    self.fail("prism", n, w, "the windows of a switch on one monitor differ in "
                              f"{', '.join(keys)}: {sorted(set(windows.values()))}")
        self.notes["prism"] = f"{count} monitors in a switch"
        # Only a switch has var-slide: an overview opening or closing has nothing to check.
        if any("var-slide" in s for _, _, _, s in self.prism_layers()):
            self.counts["prism"] = count

    def run(self):
        self.check_steps()
        self.check_rest()
        if self.seams:
            self.check_seams()
        if any(True for _ in self.prism_layers()):
            self.check_front()
            self.check_lines()
            self.check_faces()
            self.check_prism()
            for check in ("front", "lines", "faces", "prism"):
                if self.counts.get(check) == 0 and not self.failures.get(check):
                    self.failures[check].append("nothing to check: does the script still "
                                                "have the variables this check reads?")
        ok = True
        for check, note in self.notes.items():
            fails = self.failures.get(check, [])
            ok = ok and not fails
            print(f"{'FAIL' if fails else 'ok'}  {check}: {note}" +
                  (f", {len(fails)} failed" if fails else ""))
            for text in fails[:5]:
                print(f"        {text}")
            if len(fails) > 5:
                print(f"        ... and {len(fails) - 5} more")
        if self.counts.get("pops-unchecked"):
            print(f"info  pops: {self.counts['pops-unchecked']} windows drawn for one frame "
                  "only, which can't be checked")
        for kind in ("at an interruption", "at a start from rest", "inside an animation"):
            found = sorted(self.kinks.get(kind, []), reverse=True)
            if not found:
                continue
            top = ", ".join(f"{c:.1f} px/frame in frame {n} ({self.starts[n] - self.start:.3f} s)"
                            f" window {w}" for c, n, w in found[:3])
            print(f"info  kinks {kind}: {len(found)}, the largest {top}")
        if self.held:
            frames = sorted({n for _, n, _ in self.held})
            print(f"info  held: in {len(frames)} frames picom didn't advance some animations, "
                  f"up to {max(x for x, _, _ in self.held) * 1000:.0f} ms, as it does while "
                  f"the screen pauses (frames {', '.join(str(n) for n in frames[:6])})")
        return ok

    def summary(self):
        times = [self.starts_us[n] / 1e6 for n in self.order]
        gaps = sorted(b - a for a, b in zip(times, times[1:]))
        busy = [b - a for a, b in zip(times, times[1:]) if b - a < 0.5]
        triggers = defaultdict(int)
        for t, _, trigger in self.animations:
            if t >= self.starts_us[self.order[0]] / 1e6:
                triggers[trigger] += 1
        print(f"frames {len(self.order)}, animations " +
              ", ".join(f"{t} {c}" for t, c in sorted(triggers.items())))
        if busy:
            busy.sort()
            print(f"time between frames (under 0.5 s): median {busy[len(busy) // 2] * 1000:.1f} ms, "
                  f"90% {busy[int(len(busy) * 0.9)] * 1000:.1f} ms, max {busy[-1] * 1000:.1f} ms")
        elif gaps:
            print(f"time between frames: median {gaps[len(gaps) // 2] * 1000:.1f} ms")

    def plot(self, path):
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        order = self.order
        t0 = self.start
        pos, share = defaultdict(list), defaultdict(list)
        for n in order:
            t = self.starts[n] - t0
            for w, layer in self.frames[n].items():
                share[w].append((t, self.shown(layer)))
                if layer.script and "var-pos" in layer.script:
                    pos[w].append((t, layer.script["var-pos"]))
        rows = 3 if pos else 2
        fig, ax = plt.subplots(rows, 1, figsize=(12, 3 * rows), sharex=True)
        i = 0
        if pos:
            for w, pts in sorted(pos.items()):
                ax[0].plot(*zip(*pts), lw=1, label=w)
            ax[0].set_ylabel("var-pos")
            i = 1
        for w, pts in sorted(share.items()):
            ax[i].plot(*zip(*pts), lw=1, label=w)
        ax[i].set_ylabel("share shown")
        times = [self.starts[n] - t0 for n in order]
        ax[i + 1].plot(times[1:], [(b - a) * 1000 for a, b in zip(times, times[1:])], ".", ms=2)
        ax[i + 1].set_ylabel("ms since frame before")
        ax[i + 1].set_ylim(0, 100)
        ax[i + 1].set_xlabel("s")
        first = self.starts_us[order[0]] / 1e6
        for t, _, _ in self.animations:
            if t >= first:
                for a in ax:
                    a.axvline(t - first, color="0.85", lw=0.5, zorder=0)
        fig.tight_layout()
        fig.savefig(path, dpi=80)

    def strip(self, path, count, length, start=None):
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.patches import Polygon, Rectangle
        clock = {n: self.starts_us[n] / 1e6 for n in self.order}
        if start is None:
            # The second with the most frames that have a running animation.
            busy = [n for n in self.order if any(
                layer.script and layer.script["elapsed"] < layer.script["duration"]
                for layer in self.frames[n].values())] or self.order
            start = clock[max(busy, key=lambda n: sum(
                1 for m in busy if 0 <= clock[m] - clock[n] < 1))]
        span = [n for n in self.order if 0 <= clock[n] - start <= length]
        if not span:
            sys.exit(f"no frames in the {length} s from {start}")
        # The frame nearest to each of `count` times, evenly spread.
        picks = [min(span, key=lambda n: abs(clock[n] - start - i * length / max(count - 1, 1)))
                 for i in range(count)]
        x0 = min(m[0] for m in self.monitors)
        y0 = min(m[1] for m in self.monitors)
        x1 = max(m[0] + m[2] for m in self.monitors)
        y1 = max(m[1] + m[3] for m in self.monitors)
        cols = min(count, 4)
        rows = math.ceil(count / cols)
        fig, axes = plt.subplots(rows, cols, figsize=(4 * cols, 4 * rows * (y1 - y0) / (x1 - x0)))
        axes = list(axes.flat) if count > 1 else [axes]
        colors = {}
        palette = plt.get_cmap("tab10")
        for ax, n in zip(axes, picks):
            ax.set_xlim(x0, x1)
            ax.set_ylim(y1, y0)
            ax.set_aspect("equal")
            ax.axis("off")
            for m in self.monitors:
                ax.add_patch(Rectangle((m[0], m[1]), m[2], m[3], fill=False, ec="0.6", lw=0.8))
            for w, layer in self.frames[n].items():
                if layer.state != "D" or layer.opacity <= 0:
                    continue
                color = colors.setdefault(w, palette(len(colors) % 10))
                shape = clip(around(layer.corners), crop_box(layer))
                if len(shape) >= 3:
                    ax.add_patch(Polygon(shape, closed=True, fc=color,
                                         alpha=0.35 * min(layer.opacity, 1), ec=color, lw=0.8))
            ax.set_title(f"frame {n}, {clock[n] - start:.3f} s", fontsize=8)
        for ax in axes[len(picks):]:
            ax.axis("off")
        fig.tight_layout()
        fig.savefig(path, dpi=80)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--monitor", action="append", required=True,
                    help="X,Y,W,H of a monitor")
    ap.add_argument("--from", dest="start", type=float)
    ap.add_argument("--seams", action="store_true")
    ap.add_argument("--plot")
    ap.add_argument("--strip")
    ap.add_argument("--strip-frames", type=int, default=8)
    ap.add_argument("--strip-length", type=float, default=1.0)
    a = ap.parse_args()
    monitors = [[float(x) for x in m.split(",")] for m in a.monitor]
    checker = Checker(*parse(a.log), monitors, a.start, a.seams)
    checker.summary()
    ok = checker.run()
    if a.plot:
        checker.plot(a.plot)
    if a.strip:
        checker.strip(a.strip, a.strip_frames, a.strip_length, a.start)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
