# Joins KbmSweep.exe's injected movements with the app's VEYRA_KBM_TRACE rows (same clock, 100 ns)
# and prints, per mouse rate and speed: when the input thread read the first movement, when the
# right stick first left center (handed to the session), how long it took to reach 90 % of its
# steady value, the steady deflection and its spread, and how long the stick stayed off center
# after the last movement.
# Usage: py -I scripts\fork\kbm-latency.py <sweep.csv> <trace.csv>
import csv, statistics, sys
from collections import defaultdict

def rows(path):
    with open(path, newline='') as f:
        return [{k: int(v) for k, v in r.items()} for r in csv.DictReader(f)]

moves, trace = rows(sys.argv[1]), rows(sys.argv[2])
sweeps = defaultdict(list)
for m in moves:
    sweeps[m['sweep']].append(m)

ms = lambda t: t / 10000
results = defaultdict(list)
for n, ms_ in sorted(sweeps.items()):
    first, last = ms_[0]['t100ns'], ms_[-1]['t100ns']
    sign = 1 if ms_[0]['dx'] > 0 else -1
    window = [r for r in trace if first - 20000 <= r['t100ns'] <= last + 1000000]
    read = next((r for r in window if r['t100ns'] >= first and r['dx'] != 0), None)
    moved = next((r for r in window if r['t100ns'] >= first and r['sent'] and r['rx'] * sign > 0), None)
    steady = [r['rx'] * sign / 32767 for r in window if first + 500000 <= r['t100ns'] <= last]
    if any(r['device'] for r in window):
        print(f"sweep {n}: a real mouse moved during it; left out")
        continue
    if not read or not moved or not steady:
        print(f"sweep {n}: incomplete (read={read is not None} moved={moved is not None} steady={len(steady)})")
        continue
    mean = statistics.mean(steady)
    reach = next((r for r in window if r['t100ns'] >= first and r['rx'] * sign / 32767 >= 0.9 * mean), None)
    stop = next((r for r in window if r['t100ns'] > last and r['rx'] == 0 and r['sent']), None)
    results[(ms_[0]['rate'], ms_[0]['speed'])].append({
        'read': ms(read['t100ns'] - first), 'send': ms(moved['t100ns'] - first),
        'reach90': ms(reach['t100ns'] - first) if reach else float('nan'),
        'mean': mean, 'sd': statistics.pstdev(steady), 'min': min(steady), 'max': max(steady),
        'stop': ms(stop['t100ns'] - last) if stop else float('nan'),
        'sends': sum(1 for r in window if r['sent'] and first <= r['t100ns'] <= last) / max(ms(last - first), 1) * 1000,
    })

print(f"{'rate':>5} {'speed':>6} {'n':>2} | {'read ms':>7} {'1st send':>8} {'90% at':>7} {'stop':>6} | {'stick':>6} {'sd':>6} {'min':>6} {'max':>6} | {'sends/s':>7}")
allSend = []
for (rate, speed), rs in sorted(results.items()):
    avg = lambda k: statistics.mean(r[k] for r in rs)
    allSend += [r['send'] for r in rs]
    print(f"{rate:>5} {speed:>6} {len(rs):>2} | {avg('read'):7.2f} {avg('send'):8.2f} {avg('reach90'):7.2f} {avg('stop'):6.1f} | "
          f"{avg('mean'):6.3f} {avg('sd'):6.3f} {avg('min'):6.3f} {avg('max'):6.3f} | {avg('sends'):7.0f}")
if allSend:
    allSend.sort()
    print(f"injection to first stick send ms: p50={allSend[len(allSend)//2]:.2f} max={allSend[-1]:.2f} sweeps={len(allSend)}")
