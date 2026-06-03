"""Unified benchmark harness: the single-N comparison and the scaling sweep
behind one entry point, with optional High-Performance power management that
restores your plan afterward (even on error / Ctrl-C).

  python bench/run.py single [N]        # bench.exe N + bench_py.py + chart.py
                                        #   -> results.csv, comparison.png, results.md
  python bench/run.py scaling [Ns...]   # full geometric sweep (or given Ns)
                                        #   -> scaling.csv, scaling.png
  python bench/run.py all               # scaling sweep, then the single-N comparison
  python bench/run.py plot              # re-render both charts from existing CSVs (no timing)

  --high-perf     set the Windows High Performance power plan for the run and
                  restore the previous plan when done. bench.exe already pins to a
                  P-core by default (see bench/README.md); this adds the clock-
                  stability half of the trustworthy-throughput recipe.
  --reps N        best-of-N on both sides (default 3; sets bench.exe arg + BENCH_REPS).

This is a thin orchestrator: it shells out to bench.exe, bench_py.py, chart.py and
scaling.py, which all remain usable on their own. Build bench.exe first (see
bench/README.md).
"""
import argparse, os, re, subprocess, sys
from contextlib import contextmanager

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BENCH_EXE = os.path.join(HERE, "bench.exe")
HIGH_PERF_GUID = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c"  # Windows "High performance"


def _sh(args, **kw):
    """Run a subprocess from the repo root, inheriting stdio, raising on failure."""
    return subprocess.run(args, cwd=ROOT, check=True, **kw)


def _powercfg(*args):
    return subprocess.run(["powercfg", *args], capture_output=True, text=True)


def _active_scheme():
    out = _powercfg("/getactivescheme").stdout
    m = re.search(r"GUID:\s*([0-9a-fA-F-]+)", out)
    return m.group(1) if m else None


@contextmanager
def high_performance(enabled):
    """Activate High Performance for the duration, restore the prior plan after.
    No-op off Windows or when not enabled. Restore runs in finally so an error or
    Ctrl-C still puts your plan back."""
    if not enabled or os.name != "nt":
        if enabled:
            print("[run] --high-perf ignored: not on Windows", flush=True)
        yield
        return
    orig = _active_scheme()
    if HIGH_PERF_GUID not in _powercfg("/list").stdout:
        _powercfg("/duplicatescheme", HIGH_PERF_GUID)  # create from template if absent
    _powercfg("/setactive", HIGH_PERF_GUID)
    print(f"[run] power plan -> High Performance (was {orig})", flush=True)
    try:
        yield
    finally:
        if orig:
            _powercfg("/setactive", orig)
            print(f"[run] power plan restored -> {orig}", flush=True)


def _env(reps):
    return dict(os.environ, BENCH_REPS=str(reps))


def single(n, reps):
    """One canonical comparison at N: ticks side, then pyarrow side, then render."""
    if not os.path.exists(BENCH_EXE):
        raise SystemExit(f"{BENCH_EXE} not found — build it first (see bench/README.md)")
    print(f"[run] single-N comparison: N={n:,} reps={reps}", flush=True)
    _sh([BENCH_EXE, str(n), str(reps)])                       # results.csv + out.csv
    _sh([sys.executable, os.path.join(HERE, "bench_py.py")], env=_env(reps))
    _sh([sys.executable, os.path.join(HERE, "chart.py")])     # comparison.png + results.md
    print("[run] single-N done -> bench/comparison.png, bench/results.md", flush=True)


def scaling(ns, reps):
    print(f"[run] scaling sweep: {'default Ns' if not ns else ns}  reps={reps}", flush=True)
    _sh([sys.executable, os.path.join(HERE, "scaling.py"), *[str(x) for x in ns]],
        env=_env(reps))
    print("[run] scaling done -> bench/scaling.csv, bench/scaling.png", flush=True)


def main():
    ap = argparse.ArgumentParser(description="unified ticksio benchmark harness")
    ap.add_argument("mode", choices=["single", "scaling", "all", "plot"])
    ap.add_argument("ns", nargs="*", help="row counts (scaling: sweep points; single: first is N)")
    ap.add_argument("--high-perf", action="store_true", help="High Performance plan for the run, then restore")
    ap.add_argument("--reps", type=int, default=3, help="best-of-N on both sides (default 3)")
    args = ap.parse_args()

    ns = [int(float(x)) for x in args.ns]  # accept 5e6 style

    if args.mode == "plot":  # render only, no timing, no power changes
        _sh([sys.executable, os.path.join(HERE, "scaling.py"), "plot"])
        _sh([sys.executable, os.path.join(HERE, "chart.py")])
        print("[run] re-rendered scaling.png + comparison.png from existing CSVs", flush=True)
        return

    with high_performance(args.high_perf):
        if args.mode in ("scaling", "all"):
            scaling(ns, args.reps)
        if args.mode in ("single", "all"):
            single(ns[0] if ns else 5_000_000, args.reps)


if __name__ == "__main__":
    main()
