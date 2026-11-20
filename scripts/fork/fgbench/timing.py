# Latency and GPU cost of a benchmark run's log (run.log): the median, over the steady 1 s windows,
# of the engine's own capture-timing P95s, plus the guard's GPU time a real frame.
# Usage: py -I timing.py <run dir> [<run dir> ...]
import re
import statistics
import sys

KEYS = ("callbackToPresentReturnP95Ms", "gpuReadyP95Ms", "processCpuP95Ms", "gpuFlowP95Ms", "gpuFgBatchP95Ms")


def main():
    for run in sys.argv[1:]:
        text = open(f"{run}/run.log", encoding="utf-8", errors="replace").read()
        windows = []
        last = None
        for line in text.splitlines():
            if "[capture-timing]" not in line or "revision=" not in line:
                continue
            values = dict(re.findall(r"(\w+)=([-\d.]+)", line))
            if float(values.get("processed", 0)) < 60:  # warm-up
                continue
            if last is not None:
                values["expiredPerS"] = float(values["expiredGenerated"]) - last
            last = float(values["expiredGenerated"])
            windows.append(values)
        windows = windows[1:-1]
        row = {k: statistics.median(float(w[k]) for w in windows) for k in KEYS}
        row["expiredPerS"] = statistics.median(w.get("expiredPerS", 0) for w in windows)
        guard = [float(x) for x in re.findall(r"gpuMsPerRealFrame=([\d.]+)", text)]
        holds = re.findall(r"session pairs=(\d+) holds=(\d+) globalBlend=(\d+)", text)
        print(run, f"windows={len(windows)}", " ".join(f"{k}={v:.3f}" for k, v in row.items()),
              f"guardGpuMs={statistics.mean(guard):.3f}" if guard else "guardGpuMs=-", f"pairs/holds/global={holds[-1] if holds else '-'}")


if __name__ == "__main__":
    main()
