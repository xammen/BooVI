# Writes the benchmark's 30 fps source (every fourth 120 fps ground-truth picture) as 1080p HEVC,
# the format a PS5 stream arrives in, plus clip.json for the measure script.
# Usage: py -I make_clip.py <out dir> [--crf 12]
import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fgclip  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--crf", type=int, default=12)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    clip = fgclip.Clip()
    path = os.path.join(args.out, "fgbench30.mp4")
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgr24", "-s", f"{fgclip.W}x{fgclip.H}",
           "-r", "30", "-i", "-", "-c:v", "libx265", "-preset", "medium", "-crf", str(args.crf),
           "-x265-params", "log-level=error", "-pix_fmt", "yuv420p", "-colorspace", "bt709", "-color_primaries", "bt709",
           "-color_trc", "bt709", "-color_range", "tv", "-tag:v", "hvc1", path]
    enc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    for i in range(0, fgclip.FRAMES, 4):
        enc.stdin.write(clip.render(i).tobytes())
    enc.stdin.close()
    if enc.wait():
        raise SystemExit("ffmpeg failed")
    with open(os.path.join(args.out, "clip.json"), "w") as f:
        json.dump(dict(fgclip.metadata(), source=path, source_fps=30, crf=args.crf), f, indent=1)
    print(path)


if __name__ == "__main__":
    main()
