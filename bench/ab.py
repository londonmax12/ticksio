"""A/B interleaved benchmark — the trustworthy way to compare two ticks builds.

best-of-N (min over reps) removes transient jitter *within* one process, but it
cannot remove the ~20% run-to-run drift in the machine's baseline write speed
*between* processes (thermal + OS disk-writeback contention; measured, and
unchanged by REPS=3 vs REPS=10 — see bench/README.md). So a stored number from
an earlier session is not a valid baseline: the drift will swamp a real
10-15% optimization.

This driver does the only thing that beats the drift: it runs both binaries
INTERLEAVED in one session — A,B,A,B,... — so each round's two measurements see
the same machine state, and the drift cancels in the *paired* delta (B-A per
round). It reports each side's median + min..max band and, more importantly, the
paired delta with its own band and a sign-test.

  python bench/ab.py                              # A==B noise floor (min detectable effect), N=5M
  python bench/ab.py --a old.exe --b new.exe      # compare two builds
  python bench/ab.py --metric "ticks zstd:write" --n 5000000 --rounds 12

The metric is `format:metric[:variant]`, matching the rows bench.exe writes to
bench/results.csv, e.g. "ticks zstd:write", "ticks none:read:scan",
"ticks zstd:insert:100000". Each invocation runs with --no-csv (drops the 120 MB
out.csv writeback confounder) and BENCH_SETTLE_MS settle between reps; the
per-invocation value is itself best-of-REPS, and the band is over `rounds`
invocations. Build bench.exe first (see bench/README.md).
"""
import argparse, csv, os, statistics as st, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_EXE = os.path.join(HERE, "bench.exe")
RESULTS = os.path.join(HERE, "results.csv")


def parse_metric(spec):
    parts = spec.split(":")
    if len(parts) == 2:
        return parts[0], parts[1], ""
    if len(parts) == 3:
        return parts[0], parts[1], parts[2]
    raise SystemExit(f"--metric must be 'format:metric[:variant]', got {spec!r}")


def run_once(exe, n, reps, settle_ms, fmt, metric, variant):
    """Run one bench.exe invocation (--no-csv) and return the requested metric."""
    env = dict(os.environ, BENCH_SETTLE_MS=str(settle_ms))
    subprocess.run([exe, str(n), str(reps), "--no-csv"], cwd=ROOT, check=True,
                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for r in csv.DictReader(open(RESULTS)):
        if r["format"] == fmt and r["metric"] == metric and r["variant"] == variant:
            return float(r["value"])
    raise SystemExit(f"metric {fmt}:{metric}:{variant} not found in {RESULTS}")


def band(label, vals, n):
    tp = [n / v / 1e6 for v in vals]
    med, lo, hi = st.median(vals), min(vals), max(vals)
    spread = (hi - lo) / lo * 100
    cv = (st.pstdev(vals) / st.mean(vals) * 100) if len(vals) > 1 else 0.0
    print(f"  {label:<5} median {med*1e3:8.3f} ms  ({n/med/1e6:6.1f} M/s)   "
          f"band [{lo*1e3:.3f}, {hi*1e3:.3f}] ms   spread {spread:5.1f}%   CV {cv:4.1f}%")
    return med


def main():
    ap = argparse.ArgumentParser(description="interleaved A/B ticks benchmark")
    ap.add_argument("--a", default=DEFAULT_EXE, help="baseline bench.exe")
    ap.add_argument("--b", default=DEFAULT_EXE, help="candidate bench.exe (default: same as A = noise floor)")
    ap.add_argument("--n", type=lambda s: int(float(s)), default=5_000_000)
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--reps", type=int, default=3, help="best-of-REPS inside each invocation")
    ap.add_argument("--settle-ms", type=int, default=50)
    ap.add_argument("--metric", default="ticks zstd:write")
    args = ap.parse_args()

    for p in (args.a, args.b):
        if not os.path.exists(p):
            raise SystemExit(f"{p} not found — build it first (see bench/README.md)")
    fmt, metric, variant = parse_metric(args.metric)
    same = os.path.abspath(args.a) == os.path.abspath(args.b)

    print(f"A/B  metric={args.metric}  N={args.n:,}  rounds={args.rounds}  "
          f"reps={args.reps}  settle={args.settle_ms}ms"
          + ("   [A==B: NOISE FLOOR]" if same else f"\n  A = {args.a}\n  B = {args.b}"),
          flush=True)

    a_vals, b_vals, deltas = [], [], []
    for i in range(args.rounds):
        a = run_once(args.a, args.n, args.reps, args.settle_ms, fmt, metric, variant)
        b = run_once(args.b, args.n, args.reps, args.settle_ms, fmt, metric, variant)
        a_vals.append(a); b_vals.append(b); deltas.append(b - a)
        print(f"  round {i+1:>2}/{args.rounds}:  A {a*1e3:8.3f} ms   B {b*1e3:8.3f} ms   "
              f"B-A {(b-a)*1e3:+8.3f} ms ({(b-a)/a*100:+5.1f}%)", flush=True)

    print("\nper-side (median + run-to-run band over rounds):")
    ma = band("A", a_vals, args.n)
    mb = band("B", b_vals, args.n)

    # Paired delta: drift cancels here, so this is the trustworthy signal.
    dpct = [d / a * 100 for d, a in zip(deltas, a_vals)]
    med_d, lo_d, hi_d = st.median(dpct), min(dpct), max(dpct)
    b_faster = sum(1 for d in deltas if d < 0)
    print("\npaired delta  B-A  (per round — drift-cancelled, the number to trust):")
    print(f"  median {med_d:+.1f}%   band [{lo_d:+.1f}%, {hi_d:+.1f}%]   "
          f"B faster in {b_faster}/{args.rounds} rounds")

    if same:
        worst = max(abs(lo_d), abs(hi_d))
        print(f"\n  NOISE FLOOR: with identical binaries the paired delta still spans "
              f"±{worst:.1f}%.\n  Treat that as the minimum effect size you can trust; "
              f"smaller 'wins' are noise.")
    else:
        # crossing zero => not resolved above this session's paired noise
        resolved = (lo_d > 0) or (hi_d < 0)
        verdict = (f"B is {'faster' if med_d < 0 else 'slower'} by ~{abs(med_d):.1f}% (median)"
                   if resolved else
                   "INCONCLUSIVE — the delta band crosses 0; the effect is within paired noise")
        print(f"\n  VERDICT: {verdict}.")


if __name__ == "__main__":
    main()
