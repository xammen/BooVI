# Compares the frames a benchmark run handed to presentation (run-bench.ps1) with the 120 fps truth.
# Usage: py -I measure.py <run dir> [--json out.json] [--keep]   (deletes the raw dump unless --keep)
# Per segment (slow, fast, flick, aim, veryfast) and frame kind (real, generated):
#   psnr / ssim on the full frame, the crosshair box, the HUD boxes (corner text, timer, minimap);
#   lines: luma gradient energy the output has and the truth does not, perpendicular to the pan
#          (streaks drawn along the motion), mean over the scene outside the HUD, grey levels;
#   edge: the same excess in any direction.
# best: the same against the truth picture (within 4 of 120 fps) the frame matches best, so a
#   frame shown at the wrong time (a hold) counts as clean; "off" is how far that picture is.
# With the FG motion field dumped (frames/flow), also its end-point error against the true motion of
# the far and near layers (pixels), and the share of pixels more than 4 px off.
import argparse
import glob
import json
import math
import multiprocessing
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fgclip  # noqa: E402

SRC_PERIOD = 1e7 / 30.0


def load_raw(path, w, h, pitch):
    a = np.fromfile(path, np.uint8)
    a = a[: pitch * h].reshape(h, pitch)[:, : w * 4].reshape(h, w, 4)
    return cv2.cvtColor(a, cv2.COLOR_RGBA2BGR)


def ssim_map(a, b):
    a = a.astype(np.float32)
    b = b.astype(np.float32)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mu_a, mu_b = cv2.GaussianBlur(a, (11, 11), 1.5), cv2.GaussianBlur(b, (11, 11), 1.5)
    va = cv2.GaussianBlur(a * a, (11, 11), 1.5) - mu_a ** 2
    vb = cv2.GaussianBlur(b * b, (11, 11), 1.5) - mu_b ** 2
    cov = cv2.GaussianBlur(a * b, (11, 11), 1.5) - mu_a * mu_b
    return ((2 * mu_a * mu_b + c1) * (2 * cov + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (va + vb + c2))


def boxes_mask(boxes, shape):
    m = np.zeros(shape, bool)
    for x, y, w, h in boxes:
        m[y:y + h, x:x + w] = True
    return m


class Metrics:
    def __init__(self):
        h, w = fgclip.H, fgclip.W
        self.cross = boxes_mask([fgclip.CROSSHAIR_BOX], (h, w))
        self.hud = boxes_mask(fgclip.HUD_BOXES.values(), (h, w))
        self.scene = ~(self.cross | self.hud)

    def frame(self, out, gt, velocity):
        diff2 = (out.astype(np.float32) - gt.astype(np.float32)) ** 2
        mse = diff2.mean(-1)
        lo, lg = cv2.cvtColor(out, cv2.COLOR_BGR2GRAY), cv2.cvtColor(gt, cv2.COLOR_BGR2GRAY)
        ss = ssim_map(lo, lg)
        r = {}
        for name, m in (("full", None), ("cross", self.cross), ("hud", self.hud)):
            r[f"mse_{name}"] = float(mse.mean() if m is None else mse[m].mean())
            r[f"ssim_{name}"] = float(ss.mean() if m is None else ss[m].mean())
        gxo, gyo = cv2.Sobel(lo, cv2.CV_32F, 1, 0, ksize=3), cv2.Sobel(lo, cv2.CV_32F, 0, 1, ksize=3)
        gxg, gyg = cv2.Sobel(lg, cv2.CV_32F, 1, 0, ksize=3), cv2.Sobel(lg, cv2.CV_32F, 0, 1, ksize=3)
        vx, vy = velocity
        n = math.hypot(vx, vy)
        px, py = (-vy / n, vx / n) if n > 1e-6 else (0.0, 1.0)
        perp_o, perp_g = np.abs(gxo * px + gyo * py), np.abs(gxg * px + gyg * py)
        r["lines"] = float(np.maximum(perp_o - perp_g, 0)[self.scene].mean() / 8.0)
        mag_o, mag_g = np.hypot(gxo, gyo), np.hypot(gxg, gyg)
        r["edge"] = float(np.maximum(mag_o - mag_g, 0)[self.scene].mean() / 8.0)
        return r


def flow_rows(frames, clip, metrics):
    rows = []
    folder = os.path.join(frames, "flow")
    if not os.path.isdir(folder):
        return rows
    h, w = fgclip.H, fgclip.W
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    for path in sorted(glob.glob(os.path.join(folder, "b*_a*_flow.raw"))):
        name = os.path.basename(path)[:-4]
        b, a, _ = name.split("_", 2)
        kb, ka = round(int(b[1:]) / SRC_PERIOD), round(int(a[1:]) / SRC_PERIOD)
        gb, ga = 4 * kb, 4 * ka
        if not (0 <= ga < gb < fgclip.FRAMES):
            continue
        raw = np.fromfile(path, np.float16)
        pitch = raw.size // h
        f = raw.reshape(h, pitch)[:, : w * 2].reshape(h, w, 2).astype(np.float32)
        dx, dy = clip.xs[gb] - clip.xs[ga], clip.ys[gb] - clip.ys[ga]
        # current -> previous: the picture B at p shows what A showed at p + m.
        nx, ny = clip.xs[gb] * fgclip.NEAR_PARALLAX, clip.ys[gb] * 1.15
        alpha = cv2.remap(clip.alpha, gx + np.float32(nx), gy + np.float32(ny), cv2.INTER_LINEAR, borderMode=cv2.BORDER_WRAP) > 200
        tx = np.where(alpha, dx * fgclip.NEAR_PARALLAX, dx).astype(np.float32)
        ty = np.where(alpha, dy * 1.15, dy).astype(np.float32)
        epe = np.hypot(f[..., 0] - tx, f[..., 1] - ty)
        r = {"name": name, "gt": gb, "segment": fgclip.segment_of(ga + 2), "speed": float(math.hypot(dx, dy))}
        for layer, m in (("far", ~alpha & metrics.scene), ("near", alpha & metrics.scene)):
            r[f"epe_{layer}"] = float(epe[m].mean()) if m.any() else float("nan")
            r[f"bad_{layer}"] = float((epe[m] > 4).mean()) if m.any() else float("nan")
        hud = metrics.hud | metrics.cross
        r["hud_motion"] = float(np.hypot(f[..., 0], f[..., 1])[hud].mean())
        rows.append(r)
    return rows


def psnr(mse):
    return 99.0 if mse <= 1e-9 else 10 * math.log10(255.0 ** 2 / mse)


def summarize(rows):
    out = {}
    keys = {(r["segment"], r["kind"]) for r in rows}
    for seg, kind in sorted(keys):
        sel = [r for r in rows if r["segment"] == seg and r["kind"] == kind]
        s = {"n": len(sel)}
        for name in ("full", "cross", "hud"):
            s[f"psnr_{name}"] = round(psnr(np.mean([r[f"mse_{name}"] for r in sel])), 2)
            s[f"ssim_{name}"] = round(float(np.mean([r[f"ssim_{name}"] for r in sel])), 4)
            # the worst frame matters for a visible ghost
            s[f"psnr_{name}_p5"] = round(float(np.percentile([psnr(r[f"mse_{name}"]) for r in sel], 5)), 2)
        s["lines"] = round(float(np.mean([r["lines"] for r in sel])), 3)
        s["lines_p95"] = round(float(np.percentile([r["lines"] for r in sel], 95)), 3)
        s["edge"] = round(float(np.mean([r["edge"] for r in sel])), 3)
        s["psnr_best"] = round(psnr(np.mean([r["mse_best"] for r in sel])), 2)
        s["ssim_best"] = round(float(np.mean([r["ssim_best"] for r in sel])), 4)
        s["lines_best"] = round(float(np.mean([r["lines_best"] for r in sel])), 3)
        s["off"] = round(float(np.mean([abs(r["off"]) for r in sel])), 2)
        out[f"{seg}/{kind}"] = s
    return out


_clip = _metrics = None


def _init():
    global _clip, _metrics
    _clip, _metrics = fgclip.Clip(), Metrics()


def _frame_row(job):
    path, w, h, pitch = job
    name = os.path.basename(path)[:-4]
    b, a, what = name.split("_", 2)
    kb, ka = round(int(b[1:]) / SRC_PERIOD), round(int(a[1:]) / SRC_PERIOD)
    if what.startswith("real"):
        g, kind = 4 * kb, "real"
    else:
        sub, mult = (int(x) for x in what[1:].split("of"))
        span = 4 * (kb - ka)
        if span <= 0 or (sub * span) % mult:
            return None
        g, kind = 4 * ka + sub * span // mult, "gen"
    if not 0 <= g < fgclip.FRAMES:
        return None
    out = load_raw(path, w, h, pitch)
    gt = _clip.render(g)
    r = _metrics.frame(out, gt, fgclip.velocity(g / fgclip.FPS))
    small = cv2.resize(out, None, fx=0.25, fy=0.25, interpolation=cv2.INTER_AREA).astype(np.float32)
    best, best_err, best_img = g, None, gt
    for c in range(max(0, g - 4), min(fgclip.FRAMES, g + 5)):
        img = gt if c == g else _clip.render(c)
        err = ((cv2.resize(img, None, fx=0.25, fy=0.25, interpolation=cv2.INTER_AREA).astype(np.float32) - small) ** 2).mean()
        if best_err is None or err < best_err:
            best, best_err, best_img = c, err, img
    b = _metrics.frame(out, best_img, fgclip.velocity(best / fgclip.FPS))
    r.update(mse_best=b["mse_full"], ssim_best=b["ssim_full"], lines_best=b["lines"], off=best - g)
    segment = fgclip.segment_of(g)
    if segment == "flick":  # the flicks themselves, and the near-still moments between them
        segment = "flick" if math.hypot(*fgclip.velocity(g / fgclip.FPS)) > 20 else "between"
    r.update(name=name, gt=g, kind=kind, segment=segment)
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--json")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    frames = os.path.join(args.run, "frames")
    header = [l for l in open(os.path.join(frames, "index.csv")) if l.startswith("#")][-1]
    meta = dict(kv.split("=") for kv in header[1:].split())
    w, h, pitch = int(meta["width"]), int(meta["height"]), int(meta["pitch"])
    assert (w, h) == (fgclip.W, fgclip.H), f"dump is {w}x{h}"
    files = sorted(glob.glob(os.path.join(frames, "b*_a*_*.raw")))
    # A graph built before the test preset applied (another extent) is skipped by size.
    jobs = [(p, w, h, pitch) for p in files if os.path.getsize(p) >= pitch * h]
    with multiprocessing.Pool(min(12, os.cpu_count() or 4), initializer=_init) as pool:
        rows = [r for r in pool.map(_frame_row, jobs, chunksize=4) if r]
    clip, metrics = fgclip.Clip(), Metrics()
    summary = summarize(rows)
    frows = flow_rows(frames, clip, metrics)
    flow = {}
    for seg in sorted({r["segment"] for r in frows}):
        sel = [r for r in frows if r["segment"] == seg]
        flow[seg] = {k: round(float(np.nanmean([r[k] for r in sel])), 3) for k in ("speed", "epe_far", "bad_far", "epe_near", "bad_near", "hud_motion")}
        flow[seg]["n"] = len(sel)
    result = {"run": args.run, "frames": len(rows), "summary": summary, "flow": flow, "rows": rows, "flow_rows": frows}
    with open(args.json or os.path.join(args.run, "metrics.json"), "w") as f:
        json.dump(result, f, indent=1)
    for k, s in summary.items():
        print(f"{k:16s} n={s['n']:4d} psnr full/cross/hud {s['psnr_full']:6.2f} {s['psnr_cross']:6.2f} {s['psnr_hud']:6.2f}"
              f"  ssim {s['ssim_full']:.4f} {s['ssim_cross']:.4f} {s['ssim_hud']:.4f}  lines {s['lines']:.3f} (p95 {s['lines_p95']:.3f})"
              f"  best {s['psnr_best']:6.2f} {s['ssim_best']:.4f} lines {s['lines_best']:.3f} off {s['off']:.2f}")
    for seg, f in flow.items():
        print(f"flow {seg:9s} n={f['n']:3d} speed {f['speed']:6.1f}px  epe far {f['epe_far']:6.2f} (>4px {f['bad_far']:.2f})"
              f"  near {f['epe_near']:6.2f} (>4px {f['bad_near']:.2f})  motion on HUD {f['hud_motion']:6.2f}px")
    if not args.keep:
        for path in files + glob.glob(os.path.join(frames, "flow", "*.raw")):
            os.remove(path)


if __name__ == "__main__":
    main()
