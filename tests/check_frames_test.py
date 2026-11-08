#!/usr/bin/env python3
# Unit tests for check-frames.py: synthetic frame logs with known jumps, pops, stops,
# seams and prism poses, and a simulation of the styles' motion that its allowances must
# pass. make check runs them.

import importlib.util
import io
import math
import os
import random
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("check_frames", os.path.join(HERE, "check-frames.py"))
cf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cf)

MONITOR = [0.0, 0.0, 1000.0, 800.0]


def bezier(p1x, p1y, p2x, p2y):
    def f(x):
        lo, hi = 0.0, 1.0
        for _ in range(60):
            t = (lo + hi) / 2
            bx = 3 * (1 - t) ** 2 * t * p1x + 3 * (1 - t) * t * t * p2x + t ** 3
            lo, hi = (t, hi) if bx < x else (lo, t)
        t = (lo + hi) / 2
        return 3 * (1 - t) ** 2 * t * p1y + 3 * (1 - t) * t * t * p2y + t ** 3
    return f


IN_OUT = bezier(0.45, 0, 0.55, 1)
CURVES = [IN_OUT, bezier(0.25, 0.1, 0.25, 1), bezier(0.65, 0, 0.35, 1),
          bezier(0.2, 0.7, 0.3, 1), bezier(0.5, 0, 0.8, 0.3), lambda x: x]


class Log:
    """Writes a frame timing log with layout records."""

    def __init__(self):
        self.lines = ["# picom frame timing log, see src/frame_timing.h"]
        self.frame = 0

    def add(self, t, windows, started=(), handovers=None):
        """A frame at t seconds. windows: {id: dict(box, corners, opacity, crop, state,
        script)}; started: ids whose animation starts in this frame; handovers: {id:
        script} of the animations those interrupt, as they hand over (X records)."""
        self.frame += 1
        for w in started:
            self.lines.append(f"A,{int(t * 1e6)},{w},show")
        for w, s in (handovers or {}).items():
            kv = ",".join(f"{k}={v:.6g}" for k, v in s.items() if k not in ("elapsed", "duration"))
            self.lines.append(f"X,{int(t * 1e6)},{w},{s['elapsed']:.6f},{s['duration']:.3f}"
                              + ("," + kv if kv else ""))
        for w, d in windows.items():
            x, y, bw, bh = d["box"]
            corners = d.get("corners") or [(x, y), (x + bw, y), (x, y + bh), (x + bw, y + bh)]
            crop = d.get("crop", (math.inf, math.inf, math.inf, math.inf))
            fields = ["W", self.frame, w, d.get("state", "D"), x, y, bw, bh]
            fields += [f"{v:.2f}" for c in corners for v in c]
            fields += [f"{d.get('opacity', 1.0):.4f}"] + [f"{v:.2f}" if math.isfinite(v) else "inf"
                                                        for v in crop]
            self.lines.append(",".join(str(f) for f in fields))
            if d.get("script") is not None:
                s = d["script"]
                kv = ",".join(f"{k}={v:.6g}" for k, v in s.items()
                              if k not in ("elapsed", "duration"))
                self.lines.append(f"E,{self.frame},{w},{s['elapsed']:.3f},{s['duration']:.3f}"
                                  + ("," + kv if kv else ""))
        us = int(t * 1e6)
        self.lines.append(f"R,{self.frame},{us},{us},0,0,{us},{us},1")

    def check(self, seams=False, monitors=(MONITOR,)):
        with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as f:
            f.write("\n".join(self.lines) + "\n")
            path = f.name
        try:
            checker = cf.Checker(*cf.parse(path), [list(m) for m in monitors], None, seams)
            out = io.StringIO()
            with redirect_stdout(out):
                checker.run()
        finally:
            os.unlink(path)
        return checker, out.getvalue()


def frame_times(start, length, hz, jitter=0.3, seed=1):
    """Frame times, in whole milliseconds like picom's animation clock."""
    rng = random.Random(seed)
    ts = [start]
    while ts[-1] < start + length:
        ts.append(ts[-1] + (1 + rng.uniform(-jitter, jitter)) / hz)
    return sorted({math.floor(t * 1000) / 1000 for t in ts})


def slide(log, ts, t0, length, curve, frm, to, box=(100, 100, 400, 300), w="0x1",
          extra=None, opacity=lambda e: 1.0, crop=None, rest_after=True, started=True):
    """A window whose offset-y goes from frm to to on curve, from t0 for length, logged
    at times ts; before t0 and after the end at rest there, as if the window manager had
    put it there."""
    def moved(dy):
        return (box[0], box[1] + int(dy), box[2], box[3])
    # picom starts an animation in a frame: its first pose is there, at elapsed 0.
    t0 = min(t for t in ts if t >= t0)
    for t in ts:
        e = t - t0
        if e < 0:
            log.add(t, {w: {"box": moved(frm)}})
            continue
        if e >= length:
            if rest_after:
                log.add(t, {w: {"box": moved(to)}})
            continue
        y = frm + (to - frm) * curve(e / length) + (extra(e) if extra else 0.0)
        script = {"elapsed": e, "duration": length, "offset-y": y}
        d = {"box": box, "script": script, "opacity": opacity(e)}
        if crop:
            c = crop(e)
            script.update({"crop-x": c[0], "crop-y": c[1], "crop-width": c[2] - c[0],
                           "crop-height": c[3] - c[1]})
            d["crop"] = (c[0], c[1], c[2] - c[0], c[3] - c[1])
        x, y0, bw, bh = box
        d["corners"] = [(x, y0 + math.floor(y)), (x + bw, y0 + math.floor(y)),
                        (x, y0 + math.floor(y) + bh), (x + bw, y0 + math.floor(y) + bh)]
        log.add(t, {w: d}, started=[w] if started and t == min(u for u in ts if u >= t0) else [])


def fails(checker, check):
    return checker.failures.get(check, [])


class Continuity(unittest.TestCase):
    def test_smooth_eased_motion_passes(self):
        for hz in (60, 175):
            log = Log()
            slide(log, frame_times(0, 0.8, hz), 0.1, 0.4, IN_OUT, 600, 0)
            checker, out = log.check()
            for check in ("jumps", "pops", "stops", "drawn", "rest"):
                self.assertFalse(fails(checker, check), out)

    def test_jump_of_three_pixels_fails(self):
        log = Log()
        slide(log, frame_times(0, 0.8, 175), 0.1, 0.4, IN_OUT, 600, 0,
              extra=lambda e: 3.0 if e > 0.2 else 0.0)
        checker, out = log.check()
        self.assertTrue(fails(checker, "jumps"), out)

    def test_jump_under_half_a_pixel_passes(self):
        log = Log()
        slide(log, frame_times(0, 0.8, 175), 0.1, 0.4, IN_OUT, 600, 0,
              extra=lambda e: 0.3 if e > 0.2 else 0.0)
        checker, out = log.check()
        self.assertFalse(fails(checker, "jumps"), out)

    def test_interruption_that_carries_the_place_passes_and_reports_the_kink(self):
        log = Log()
        ts = frame_times(0, 1.2, 175)
        t_cut = min(t for t in ts if t >= 0.3)
        t_first = min(t for t in ts if t >= 0.1)
        y_cut = 600 * (1 - IN_OUT((t_cut - t_first) / 0.4))
        slide(log, [t for t in ts if t < t_cut], 0.1, 0.4, IN_OUT, 600, 0, rest_after=False)
        slide(log, [t for t in ts if t >= t_cut], t_cut, 0.4, IN_OUT, y_cut, 0)
        checker, out = log.check()
        self.assertFalse(fails(checker, "jumps"), out)
        self.assertTrue(checker.kinks.get("at an interruption"), out)

    def test_interruption_that_starts_elsewhere_fails(self):
        log = Log()
        ts = frame_times(0, 1.2, 175)
        t_cut = min(t for t in ts if t >= 0.3)
        t_first = min(t for t in ts if t >= 0.1)
        y_cut = 600 * (1 - IN_OUT((t_cut - t_first) / 0.4))
        slide(log, [t for t in ts if t < t_cut], 0.1, 0.4, IN_OUT, 600, 0, rest_after=False)
        slide(log, [t for t in ts if t >= t_cut], t_cut, 0.4, IN_OUT, y_cut + 100, 0)
        checker, out = log.check()
        self.assertTrue(fails(checker, "jumps"), out)

    def test_stop_at_full_speed_fails(self):
        log = Log()
        slide(log, frame_times(0, 0.8, 175), 0.1, 0.4, lambda x: x, 600, 0)
        checker, out = log.check()
        self.assertTrue(fails(checker, "stops"), out)

    # Frames of a window of cube.conf (from the harness, the overview opened halfway into
    # a switch): time, corners (top left, top right, bottom left, bottom right), and
    # elapsed time of its animation, which starts in the frame at 0.18516. Its depth turns
    # back at 0.29: a bend, after which it bends on. The window's place is 1720,480,
    # 1720x1440.
    CUBE_BEND = [
        (0.0, [1720.0, 480.0, 3440.0, 480.0, 1720.0, 1920.0, 3440.0, 1920.0], 0.0),
        (0.04468, [1720.0, 497.34, 3304.35, 497.34, 1720.0, 1836.53, 3338.83, 1836.53], 0.04468),
        (0.069041, [1720.0, 514.47, 3151.82, 514.47, 1720.0, 1734.08, 3222.79, 1734.08], 0.069041),
        (0.092595, [1720.0, 525.88, 2999.08, 525.88, 1720.0, 1615.81, 3106.81, 1615.81], 0.092595),
        (0.117104, [1720.0, 523.85, 2866.2, 523.85, 1720.0, 1483.84, 3012.9, 1483.84], 0.117104),
        (0.140496, [1720.0, 506.07, 2774.85, 506.07, 1720.0, 1348.65, 2963.6, 1348.65], 0.140496),
        (0.18516, [1720.0, 452.09, 2675.68, 452.09, 1720.0, 1033.15, 2967.73, 1033.15], 0.0),
        (0.216966, [1720.0, 449.09, 2674.01, 449.09, 1720.0, 1016.59, 2972.1, 1016.59], 0.031806),
        (0.248153, [1720.0, 438.66, 2671.72, 438.66, 1720.0, 963.57, 2990.4, 963.57], 0.062993),
        (0.273792, [1720.0, 426.31, 2669.59, 426.31, 1720.0, 887.91, 3017.04, 887.91], 0.088632),
        (0.29796, [1720.0, 446.5, 2629.45, 446.5, 1720.0, 810.05, 2977.51, 810.05], 0.1128),
        (0.329524, [1720.0, 489.82, 2568.28, 489.82, 1720.0, 697.71, 2907.61, 697.71], 0.144364),
        (0.355794, [1720.0, 540.19, 2517.92, 540.19, 1720.0, 609.07, 2841.79, 609.07], 0.170634),
    ]

    def test_bend_then_bending_passes_and_a_jump_there_fails(self):
        """The cube's frames above: the two sides' curves don't meet in the step at 0.29,
        but each corner moves by what the speeds before and after allow. Going 60 px up
        in that step is a jump."""
        box = (1720, 480, 1720, 1440)
        rest = [(1720, 480), (3440, 480), (1720, 1920), (3440, 1920)]
        for jump in (False, True):
            log = Log()
            for i, (t, c, elapsed) in enumerate(self.CUBE_BEND):
                corners = [(c[k], c[k + 1] - (60 if jump and i >= 10 else 0))
                           for k in range(0, 8, 2)]
                script = {"elapsed": elapsed, "duration": 0.56 if i < 6 else 0.4}
                for (x, y), (rx, ry), name in zip(corners, rest, ("top-left", "top-right",
                                                                 "bottom-left", "bottom-right")):
                    script[f"corner-{name}-x"] = x - rx
                    script[f"corner-{name}-y"] = y - ry
                log.add(t, {"0x1": {"box": box, "corners": corners, "script": script}},
                        started=["0x1"] if i in (0, 6) else [])
            checker, out = log.check(monitors=([0.0, 480.0, 3440.0, 1440.0],))
            self.assertEqual(bool(fails(checker, "jumps")), jump, out)

    # Frames of a window of push.conf (from the harness): time, the window's top, the
    # bottom edge of its crop, and the elapsed time of its animation. The window's place is
    # 0,480, 3440x1440. It moves up 650 px a frame and an interruption starts a new
    # animation at 0.05057, which takes over from the old one at its elapsed 0.120287;
    # in between, var-pos passes 0, below which the crop cuts.
    PUSH_INTERRUPTED = [
        (0.0, 1765.37, 1920.0, 0.069717),
        (0.021483, 1108.64, 1920.0, 0.0912),
        (0.05057, 457.66, 1845.55, 0.0),
        (0.07059, 458.79, 1849.29, 0.02002),
        (0.094547, 461.65, 1858.84, 0.043977),
        (0.114228, 464.88, 1869.6, 0.063658),
        (0.133074, 467.99, 1879.98, 0.082504),
        (0.151886, 470.7, 1889.0, 0.101316),
        (0.170389, 472.89, 1896.3, 0.119819),
    ]

    @staticmethod
    def push_script(top, bottom, elapsed):
        return {"elapsed": elapsed, "duration": 0.3, "offset-y": top - 480, "crop-x": 0.0,
                "crop-y": 480.0, "crop-width": 3440.0, "crop-height": bottom - 480}

    def push_log(self, handed_top=457.66, first_top=457.66):
        box = (0, 480, 3440, 1440)
        log = Log()
        for i, (t, top, bottom, elapsed) in enumerate(self.PUSH_INTERRUPTED):
            if i == 2:
                top = first_top
            corners = [(0, top), (3440, top), (0, top + 1440), (3440, top + 1440)]
            log.add(t, {"0x1": {"box": box, "corners": corners,
                                "script": self.push_script(top, bottom, elapsed),
                                "crop": (0.0, 480.0, 3440.0, bottom - 480)}},
                    started=["0x1"] if i == 2 else [],
                    handovers={"0x1": self.push_script(handed_top, 1845.55, 0.120287)}
                    if i == 2 else None)
        return log.check(monitors=([0.0, 480.0, 3440.0, 1440.0],))

    def test_crop_edge_that_starts_moving_before_a_handover_passes(self):
        """The push frames above: the crop edge stood still in the old animation's frames,
        and starts to cut in the part no frame shows before the old one hands over, at the
        speed the window moves. The new animation starts where the old one handed over."""
        checker, out = self.push_log()
        self.assertFalse(fails(checker, "jumps"), out)

    def test_a_new_animation_that_starts_elsewhere_than_the_handover_fails(self):
        """The same, but the new animation starts 3 px from where the old one handed over,
        while the window moved 650 px a frame: that is a jump."""
        checker, out = self.push_log(first_top=460.66)
        self.assertTrue(fails(checker, "jumps"), out)

    def test_opacity_jump_fails(self):
        log = Log()
        slide(log, frame_times(0, 0.8, 175), 0.1, 0.4, IN_OUT, 600, 0,
              opacity=lambda e: 0.6 if e > 0.2 else 1.0)
        checker, out = log.check()
        self.assertTrue(fails(checker, "jumps"), out)

    def test_wipe_edge_continuous_passes_and_jump_fails(self):
        def wipe(edge_jump):
            def crop(e):
                edge = 800 * IN_OUT(min(e / 0.4, 1)) + (300 if edge_jump and e > 0.2 else 0)
                return (0, 0, 1000, min(edge, 800))
            return crop
        for jump in (False, True):
            log = Log()
            ts = frame_times(0, 0.8, 175)
            slide(log, [t for t in ts if t >= 0.1], 0.1, 0.4, IN_OUT, 0, 0,
                  box=(0, 0, 1000, 800), crop=wipe(jump))
            checker, out = log.check()
            self.assertEqual(bool(fails(checker, "jumps")), jump, out)


class Pops(unittest.TestCase):
    def window(self, opacity, y=100):
        return {"box": (100, y, 400, 300), "opacity": opacity,
                "script": {"elapsed": 0.0, "duration": 1.0}}

    def test_window_that_appears_at_full_opacity_fails(self):
        log = Log()
        ts = frame_times(0, 0.3, 175)
        other = {"0x9": {"box": (600, 100, 300, 300)}}
        for t in ts:
            log.add(t, other if t < 0.1 else {**other, "0x1": {"box": (100, 100, 400, 300)}})
        checker, out = log.check()
        self.assertTrue(fails(checker, "pops"), out)

    def test_fade_in_from_zero_passes(self):
        log = Log()
        ts = frame_times(0, 0.5, 175)
        t_start = min(t for t in ts if t >= 0.1)
        other = {"0x9": {"box": (600, 100, 300, 300)}}
        for t in ts:
            e = t - t_start
            if e <= 0:
                log.add(t, {**other, "0x1": {"box": (100, 100, 400, 300), "state": "T",
                                             "opacity": 0.0,
                                             "script": {"elapsed": max(e, 0), "duration": 0.2}}}
                        if e == 0 else other, started=["0x1"] if e == 0 else [])
            elif e < 0.2:
                log.add(t, {**other, "0x1": {"box": (100, 100, 400, 300), "opacity": e / 0.2,
                                             "script": {"elapsed": e, "duration": 0.2}}})
            else:
                log.add(t, {**other, "0x1": {"box": (100, 100, 400, 300)}})
        checker, out = log.check()
        self.assertFalse(fails(checker, "pops"), out)

    def test_window_that_slides_in_from_outside_the_monitor_passes(self):
        log = Log()
        ts = frame_times(0, 0.8, 60)
        box = (100, 100, 400, 300)
        other = {"0x9": {"box": (600, 100, 300, 300)}}
        for t in ts:
            e = t - 0.1
            if e < 0:
                log.add(t, other)
                continue
            if e >= 0.4:
                log.add(t, {**other, "0x1": {"box": box}})
                continue
            y = 900 * (1 - IN_OUT(e / 0.4))
            state = "D" if 100 + y < 800 else "O"
            log.add(t, {**other, "0x1": {"box": box, "state": state,
                                "script": {"elapsed": e, "duration": 0.4, "offset-y": y}}},
                    started=["0x1"] if e == min(u for u in ts if u >= 0.1) - 0.1 else [])
        checker, out = log.check()
        self.assertFalse(fails(checker, "pops"), out)


class Seams(unittest.TestCase):
    def strip(self, gap):
        log = Log()
        for t in frame_times(0, 0.6, 60):
            e = min(max(t - 0.1, 0), 0.4)
            p = IN_OUT(e / 0.4)
            windows = {}
            for w, start in (("0x1", 0.0), ("0x2", 800.0 + gap)):
                y = start - p * (800 + gap)
                windows[w] = {"box": (0, 0, 1000, 800), "crop": (0, 0, 1000, 800),
                              "corners": [(0, y), (1000, y), (0, y + 800), (1000, y + 800)],
                              "script": {"elapsed": e, "duration": 0.4, "var-slide": 1,
                                         "offset-y": y, "crop-x": 0, "crop-y": 0,
                                         "crop-width": 1000, "crop-height": 800}}
            log.add(t, windows)
        return log.check(seams=True)

    def test_workspaces_side_by_side_pass(self):
        checker, out = self.strip(0)
        self.assertFalse(fails(checker, "seams"), out)
        self.assertTrue(checker.counts.get("seams"), out)

    def test_gap_fails(self):
        checker, out = self.strip(3)
        self.assertTrue(fails(checker, "seams"), out)

    def test_overlap_fails(self):
        checker, out = self.strip(-3)
        self.assertTrue(fails(checker, "seams"), out)


def prism_window(box, pos, sides, back, cam=1.5, along_scale=1.0, tilt=0.0, fit=1.0):
    """A window on a prism turning up and down on MONITOR, as prism-view.conf draws it."""
    mx, my = MONITOR[0] + MONITOR[2] / 2, MONITOR[1] + MONITOR[3] / 2
    side = MONITOR[3]
    h = math.pi / sides
    r = side / 2 / math.tan(h)
    d = cam * side
    a = 2 * math.pi * pos / sides + tilt
    x, y, bw, bh = box
    corners, script = [], {"elapsed": 0.1, "duration": 1.0, "var-pos": pos, "var-sides": sides,
                           "var-sideways": 0, "var-d": d, "var-back": back, "var-r": r,
                           "var-cos": math.cos(a), "var-sin": math.sin(a), "var-pan": 0,
                           "var-camera": cam, "var-fit": fit, "var-length": 1, "var-slide": 1}
    for i, name in enumerate(("top-left", "top-right", "bottom-left", "bottom-right")):
        px, py = x + (i % 2) * bw, y + (i // 2) * bh
        u, v = (py - my) * along_scale, px - mx
        k = d / (d + back + r * (1 - math.cos(a)) + u * math.sin(a))
        du = (u * math.cos(a) + r * math.sin(a)) * k - (py - my)
        dv = v * (k - 1)
        script[f"corner-{name}-x"] = dv
        script[f"corner-{name}-y"] = du
        corners.append((px + dv, py + du))
    return {"box": box, "corners": corners, "script": script,
            "crop": (MONITOR[0], MONITOR[1], MONITOR[2], MONITOR[3])}


class Prisms(unittest.TestCase):
    def one(self, windows):
        log = Log()
        log.add(0.0, windows)
        log.add(0.01, {w: {"box": d["box"]} for w, d in windows.items()})
        return log.check()

    def test_true_projection_passes(self):
        checker, out = self.one({"0x1": prism_window((0, 0, 1000, 400), 0.2, 6, 600),
                                 "0x2": prism_window((0, 400, 1000, 400), 0.2, 6, 600)})
        for check in ("lines", "faces", "front"):
            self.assertFalse(fails(checker, check), out)

    def test_window_with_the_wrong_length_fails(self):
        checker, out = self.one({"0x1": prism_window((0, 0, 1000, 800), 0.2, 6, 600,
                                                     along_scale=1.1)})
        self.assertTrue(fails(checker, "lines"), out)

    def test_windows_of_one_workspace_in_two_planes_fail(self):
        checker, out = self.one({"0x1": prism_window((0, 0, 1000, 400), 0.2, 6, 600),
                                 "0x2": prism_window((0, 400, 1000, 400), 0.2, 6, 600,
                                                     tilt=0.05)})
        self.assertTrue(fails(checker, "lines"), out)

    def test_front_side_off_the_monitor_fails_unless_fit_is_below_one(self):
        checker, out = self.one({"0x1": prism_window((0, 0, 1000, 800), 0.25, 4, 0)})
        self.assertTrue(fails(checker, "front"), out)
        checker, out = self.one({"0x1": prism_window((0, 0, 1000, 800), 0.25, 4, 0, fit=0.5)})
        self.assertFalse(fails(checker, "front"), out)


def need_star(psi, side, n, cam):
    """parts/prism.conf's var-need: the most a side turned by up to psi needs."""
    h = math.pi / n
    r, d = side / 2 / math.tan(h), cam * side
    c, s = math.cos(psi), math.sin(psi)
    ka, kb = d + r, abs(2 * r * d / side - side / 2)
    kr = math.hypot(ka, kb)
    ia, ib = r - d, 2 * r * d / side + side / 2
    ir = math.hypot(max(0, ia), ib)
    return max(0, ka * max(c, ka / kr) + kb * min(s, kb / kr) - ka,
               ia * max(c, max(0, ia) / ir) + ib * min(s, ib / ir) - ka,
               r * c + side / 2 * s - r)


def prism_motion(n, distance, side=1440, across=3440, cam=1.5):
    """The corners of a side of cube.conf's prism in a switch of distance, over x from 0
    to 1, with prism.conf's depth: what the frames of the prism styles show."""
    h = math.pi / n
    r, d = side / 2 / math.tan(h), cam * side

    def f(x):
        pos = distance * (1 - IN_OUT(x))
        frac = abs(pos - round(pos))
        back = max(need_star(h * math.sin(math.pi * frac), side, n, cam),
                   need_star(h, side, n, cam) * math.sin(math.pi * x) ** 2)
        a = 2 * math.pi * pos / n
        out = []
        for u in (-side / 2, side / 2):
            for v in (-across / 2, across / 2):
                k = d / (d + back + r * (1 - math.cos(a)) + u * math.sin(a))
                out += [(u * math.cos(a) + r * math.sin(a)) * k, v * k]
        return out
    return f


class Allowances(unittest.TestCase):
    def test_the_styles_motion_never_reads_as_a_jump(self):
        """The styles' timing curves, flat, and the corners of the prism styles with their
        depth (whose max bends), at 8 to 64 frames per animation with frame times 30%
        uneven: no step within an animation reads as a jump with no allowance beyond half
        a pixel, and none at an interruption with the old animation's last step."""
        motions = [lambda x, c=c: [1000 * c(x), 0.0] * 4 for c in CURVES]
        motions += [prism_motion(n, dist) for n in (2, 4, 6) for dist in (1, 2)]
        rng = random.Random(1)
        worst_in = worst_ahead = -math.inf
        for frames in (8, 12, 16, 24, 64):
            for f in motions:
                for _ in range(3):
                    ts = [0.0]
                    while ts[-1] < 1:
                        ts.append(min(1.0, ts[-1] + (1 + rng.uniform(-0.3, 0.3)) / frames))
                    ys = [f(t) for t in ts]
                    for i in range(4, len(ts) - 4):
                        left = cf.Side([(ts[j], ys[j]) for j in range(i - 1, i - 5, -1)], False)
                        right = cf.Side([(ts[j], ys[j]) for j in range(i, i + 4)], False)
                        inside = cf.smooth_step(left, right, ts[i - 1], ts[i], ys[i - 1], ys[i],
                                                cf.corner_distance, cf.HALF_PX + cf.ROUND_PX)
                        worst_in = max(worst_in, inside[0])
                        left.slack = 2 * max(cf.corner_steps(ys[j], ys[j - 1]) for j in range(i - 3, i + 3))
                        ahead = cf.meet(left, cf.point(ts[i], ys[i]), ts[i - 1], ts[i],
                                        cf.corner_distance)
                        worst_ahead = max(worst_ahead, ahead[0])
        self.assertLessEqual(worst_in, 0.0)
        self.assertLessEqual(worst_ahead, 0.0)


if __name__ == "__main__":
    unittest.main()
