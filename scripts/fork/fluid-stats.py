"""Pacing and latency of one stream session, from a log taken with VEYRA_VERBOSE_FRAME_LOGS=1.

Usage: py scripts/fork/fluid-stats.py <log> [--skip 8] [--label name]

Reads the per-present `pacing-sample` lines (host times in 100 ns) and the per-second
`capture-timing`, `display-latency` and `stream-cadence` lines, and prints one row:
output fps, present-interval percentiles, how long a real picture waits from its decode
callback to the return of its Present, GPU cost per source frame and scan-out delay.
--skip drops the first seconds after the first present (connection, warm-up, shader caches).
"""
import re
import sys


def pct(values, p):
    if not values:
        return float("nan")
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * p))]


def fields(line, tag):
    return dict(m.split("=", 1) for m in re.findall(r"(\w+=\S+)", line.split(tag + "]", 1)[1]))


def main():
    args = sys.argv[1:]
    skip, label = 8.0, None
    if "--skip" in args:
        skip = float(args[args.index("--skip") + 1])
    if "--label" in args:
        label = args[args.index("--label") + 1]
    path = args[0]
    presents, timing, scanout, cadence = [], [], [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        if "[pacing-sample]" in line:
            f = fields(line, "pacing-sample")
            presents.append((int(f["begin"]), int(f["end"]), f["generated"] == "true", int(f["arrival"]),
                             int(f["ready"]), int(f["pts"])))
        elif "[capture-timing]" in line:
            timing.append(fields(line, "capture-timing"))
        elif "[display-latency]" in line:
            scanout.append(fields(line, "display-latency"))
        elif "[stream-cadence]" in line:
            cadence.append(fields(line, "stream-cadence"))
    if len(presents) < 100:
        print(f"{label or path}: only {len(presents)} presents logged")
        return
    start = presents[0][0] + int(skip * 1e7)
    presents = [p for p in presents if p[0] >= start]
    # A stream that lost packets freezes until the next keyframe. That is the network, not the
    # pacing measured here: from 1 s before such a freeze to 2 s after it nothing is counted.
    freezes = [b[0] for a, b in zip(presents, presents[1:]) if b[0] - a[0] > 1000000]
    presents = [p for p in presents if not any(f - 10000000 <= p[0] <= f + 20000000 for f in freezes)]
    spans = [(a, b) for a, b in zip(presents, presents[1:]) if b[0] - a[0] < 10000000]
    seconds = sum(b[0] - a[0] for a, b in spans) / 1e7
    intervals = [(b[0] - a[0]) / 1e4 for a, b in spans]
    real = [p for p in presents if not p[2]]
    generated = [p for p in presents if p[2]]
    real_wait = [(p[1] - p[3]) / 1e4 for p in real]
    first_shown = {}
    for p in presents:   # the first picture of each source frame's group to reach the screen
        first_shown.setdefault(p[3], (p[0] - p[3]) / 1e4)
    hold = [(p[0] - p[4]) / 1e4 for p in presents if p[4] > 0]
    arrivals = sorted(set(p[3] for p in real))
    arrival_gaps = [(b - a) / 1e4 for a, b in zip(arrivals, arrivals[1:])]
    nominal = pct(intervals, .5)
    late = sum(1 for i in intervals if i > nominal * 1.5)
    tail = timing[len(timing) // 2:] or [{}]

    def med(key, rows=tail):
        vals = [float(r[key]) for r in rows if key in r]
        return pct(vals, .5)

    gpu = sum(med(k) for k in ("gpuColorP95Ms", "gpuSrP95Ms", "gpuFlowP95Ms", "gpuFgBatchP95Ms", "gpuBlitP95Ms"))
    print(f"== {label or path}")
    print(f"window {seconds:.1f} s, presents {len(presents)} ({len(real)} real, {len(generated)} generated),"
          f" stream freezes left out {len(freezes)}")
    print(f"output fps {len(presents) / seconds:.1f}   real pictures/s {len(real) / seconds:.1f}")
    print(f"present interval ms p50 {pct(intervals, .5):.2f}  p95 {pct(intervals, .95):.2f}  p99 {pct(intervals, .99):.2f}"
          f"  max {max(intervals):.2f}   gaps over 1.5x nominal {late} ({100 * late / len(intervals):.2f} %)")
    print(f"real picture: decode callback -> Present return ms p50 {pct(real_wait, .5):.2f}  p95 {pct(real_wait, .95):.2f}")
    print(f"first picture of a group on screen after its callback ms p50 {pct(list(first_shown.values()), .5):.2f}"
          f"  p95 {pct(list(first_shown.values()), .95):.2f}")
    print(f"ready -> Present begin (the hold) ms p50 {pct(hold, .5):.2f}  p95 {pct(hold, .95):.2f}")
    print(f"kept-frame arrival gap ms p50 {pct(arrival_gaps, .5):.2f}  p5 {pct(arrival_gaps, .05):.2f}  p95 {pct(arrival_gaps, .95):.2f}")
    print(f"GPU ms per source frame (colour+SR+flow+FG+blit, p95 columns) {gpu:.2f}"
          f"   SR {med('gpuSrP95Ms'):.2f} flow {med('gpuFlowP95Ms'):.2f} FG {med('gpuFgBatchP95Ms'):.2f}")
    print(f"present -> scan-out ms p50 {med('presentToScanoutP50Ms', scanout[len(scanout) // 2:] or [{}]):.2f}"
          f"  p95 {med('presentToScanoutP95Ms', scanout[len(scanout) // 2:] or [{}]):.2f}")
    if cadence:
        modes = [c.get("mode", "?") for c in cadence]
        last = cadence[-1]
        print(f"stream cadence: half {modes.count('half')} s, full {modes.count('full')} s; last second "
              f"new={last.get('newPictures')} dropped={last.get('dropped')} multiplier={last.get('multiplier')}")


if __name__ == "__main__":
    main()
