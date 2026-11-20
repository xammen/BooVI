# Side-by-side crops: truth | before | after, for the frames where "before" is worst in a region.
# Usage: py -I crops.py <before run> <after run> <out dir> [--per 2]
# Both runs need their raw frames (measure.py --keep).
import argparse
import json
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fgclip  # noqa: E402
import measure  # noqa: E402

REGIONS = {"crosshair": (fgclip.W // 2 - 110, fgclip.H // 2 - 80, 220, 160),
           "hud": (0, 0, 480, 150),
           "scene": (520, 300, 640, 360)}


def label(img, text):
    img = img.copy()
    cv2.putText(img, text, (8, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 4, cv2.LINE_AA)
    cv2.putText(img, text, (8, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 1, cv2.LINE_AA)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("out")
    ap.add_argument("--per", type=int, default=2)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    # Runs may dump different pairs: frames are matched by their time in the truth.
    before = {r["gt"]: r for r in json.load(open(os.path.join(args.before, "metrics.json")))["rows"] if r["kind"] == "gen"}
    after = {r["gt"]: r for r in json.load(open(os.path.join(args.after, "metrics.json")))["rows"] if r["kind"] == "gen"}
    common = [g for g in before if g in after]
    clip = fgclip.Clip()
    key = {"crosshair": "mse_cross", "hud": "mse_hud", "scene": "mse_full"}
    written = []
    for seg in ("slow", "fast", "flick", "aim", "veryfast"):
        for region, (x, y, w, h) in REGIONS.items():
            names = sorted((n for n in common if before[n]["segment"] == seg), key=lambda n: -before[n][key[region]])
            for n in names[: args.per]:
                gt = clip.render(n)
                b = measure.load_raw(os.path.join(args.before, "frames", before[n]["name"] + ".raw"), fgclip.W, fgclip.H, fgclip.W * 4)
                a = measure.load_raw(os.path.join(args.after, "frames", after[n]["name"] + ".raw"), fgclip.W, fgclip.H, fgclip.W * 4)
                crop = lambda im: cv2.resize(im[y:y + h, x:x + w], None, fx=2, fy=2, interpolation=cv2.INTER_NEAREST)
                pb, pa = measure.psnr(before[n][key[region]]), measure.psnr(after[n][key[region]])
                row = np.hstack([label(crop(gt), "verite 120 i/s"), label(crop(b), f"avant {pb:.1f} dB"), label(crop(a), f"apres {pa:.1f} dB")])
                path = os.path.join(args.out, f"{seg}-{region}-t{n}.png")
                cv2.imwrite(path, row)
                written.append(path)
    print("\n".join(written))


if __name__ == "__main__":
    main()
