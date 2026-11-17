#!/usr/bin/env python3
"""Summarize a BF2_LOAD_TRACE run.

    loadtrace.py <prefix> [--top N] [--frames-after S]

Reads every <prefix>-*.tsv (one per image: bf2, renddx9), nests the spans per thread and reports,
for each "phase" span (boot, engine.init, game.start, level.load):

  - wall time and the self time of every category inside it (time not covered by a nested span);
    "(untraced)" is phase time no instrumented span accounts for;
  - the segments between the marks inside it (load:* sections, part:* LoadStat phases);
  - the most expensive names per heavy category, by self time.

After each level.load it lists the long frames (> 50 ms) of the following seconds, which is where
work deferred to first use shows up.
"""

import argparse
import collections
import glob
import sys


class Span:
    __slots__ = ("tid", "t0", "t1", "cat", "bytes", "name", "self_us", "parent", "image")

    def __init__(self, tid, t0, t1, cat, nbytes, name, image):
        self.tid, self.t0, self.t1, self.cat, self.bytes, self.name, self.image = (
            tid, t0, t1, cat, nbytes, name, image)
        self.self_us = t1 - t0
        self.parent = None


def load(prefix):
    files = sorted(glob.glob(prefix + "-*.tsv"))
    if not files:
        sys.exit(f"no files match {prefix}-*.tsv")
    spans, marks = [], []
    for path in files:
        image = path[len(prefix) + 1:-4]
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("#"):
                    continue
                parts = line.rstrip("\n").split("\t", 5)
                if len(parts) < 6:
                    continue  # a row cut off by a crash
                tid, t0, t1, cat, nbytes, name = parts
                s = Span(tid, int(t0), int(t1), cat, int(nbytes), name, image)
                (marks if cat == "mark" else spans).append(s)
    return files, spans, marks


def nest(spans):
    """Set parent and self time; spans on one thread either nest or are disjoint."""
    by_tid = collections.defaultdict(list)
    for s in spans:
        by_tid[s.tid].append(s)
    for lst in by_tid.values():
        lst.sort(key=lambda s: (s.t0, -s.t1))
        stack = []
        for s in lst:
            while stack and (stack[-1].t1 <= s.t0 or stack[-1].t1 < s.t1):
                stack.pop()
            if stack:
                s.parent = stack[-1]
                stack[-1].self_us -= s.t1 - s.t0
            stack.append(s)


def ms(us):
    return f"{us / 1000.0:10.1f}"


def inside(s, w):
    return s.tid == w.tid and s.t0 >= w.t0 and s.t1 <= w.t1 and s is not w


def report_phase(w, spans, marks, origin, top):
    print(f"\n=== {w.name}  start +{(w.t0 - origin) / 1e6:.2f} s  wall {(w.t1 - w.t0) / 1e6:.2f} s ===")
    contained = [s for s in spans if inside(s, w)]
    # Phase spans count as untraced time, wherever they are nested.
    untraced = w.self_us + sum(s.self_us for s in contained if s.cat == "phase")
    by_cat = collections.defaultdict(lambda: [0, 0, 0])  # self us, count, bytes
    for s in contained:
        if s.cat == "phase":
            continue
        c = by_cat[s.cat]
        c[0] += s.self_us
        c[1] += 1
        c[2] += s.bytes
    print("  self time by category              ms     count")
    rows = sorted(by_cat.items(), key=lambda kv: -kv[1][0])
    rows.append(("(untraced)", [untraced, 0, 0]))
    rows.sort(key=lambda kv: -kv[1][0])
    for cat, (us, n, _) in rows:
        share = 100.0 * us / max(1, w.t1 - w.t0)
        print(f"    {cat:<24} {ms(us)}  {n:8d}  {share:5.1f}%")

    ms_in = sorted((m for m in marks if m.tid == w.tid and w.t0 <= m.t0 <= w.t1), key=lambda m: m.t0)
    if ms_in:
        print("  segments (mark -> next mark)       ms")
        for i, m in enumerate(ms_in):
            end = ms_in[i + 1].t0 if i + 1 < len(ms_in) else w.t1
            print(f"    {m.name:<32} {ms(end - m.t0)}")

    heavy = [cat for cat, (us, _, _) in rows if cat != "(untraced)" and us >= 0.02 * (w.t1 - w.t0)]
    for cat in heavy:
        agg = collections.defaultdict(lambda: [0, 0, 0])  # self, inclusive, count
        for s in contained:
            if s.cat == cat:
                a = agg[s.name]
                a[0] += s.self_us
                a[1] += s.t1 - s.t0
                a[2] += 1
        print(f"  top {cat} by self time           self ms   incl ms  count")
        for name, (su, iu, n) in sorted(agg.items(), key=lambda kv: -kv[1][0])[:top]:
            print(f"    {name[-48:]:<48} {ms(su)} {ms(iu)} {n:5d}")


def report_frames(level, spans, after_s, top):
    frames = [s for s in spans if s.cat == "frame" and s.tid == level.tid
              and level.t1 <= s.t0 <= level.t1 + after_s * 1e6]
    if not frames:
        return
    total = sum(f.t1 - f.t0 for f in frames)
    print(f"  long frames in the {after_s} s after {level.name}: {len(frames)}, {total / 1e3:.0f} ms total")
    for f in sorted(frames, key=lambda f: -(f.t1 - f.t0))[:top]:
        kids = collections.Counter()
        for s in spans:
            if s.tid == f.tid and f.t0 <= s.t0 and s.t1 <= f.t1 and s is not f and s.cat != "phase":
                kids[s.cat] += s.self_us
        detail = ", ".join(f"{c} {us / 1e3:.0f}" for c, us in kids.most_common(4))
        print(f"    +{(f.t0 - level.t1) / 1e6:6.2f} s {(f.t1 - f.t0) / 1e3:7.0f} ms   {detail}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefix")
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--frames-after", type=float, default=30.0)
    a = ap.parse_args()

    files, spans, marks = load(a.prefix)
    nest(spans)
    origin = min(s.t0 for s in spans + marks)
    print("files:", ", ".join(files))
    print(f"{len(spans)} spans, {len(marks)} marks")
    phases = sorted((s for s in spans if s.cat == "phase"), key=lambda s: s.t0)
    for w in phases:
        report_phase(w, spans, marks, origin, a.top)
        if w.name == "level.load":
            report_frames(w, spans, a.frames_after, a.top)


if __name__ == "__main__":
    main()
