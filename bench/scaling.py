"""Scaling benchmark: run the format comparison across a sweep of row counts and
plot how each format scales with dataset size.

  python bench/scaling.py                 # run the full sweep, then render
  python bench/scaling.py 1e5 1e6 1e7      # custom row counts
  python bench/scaling.py plot             # re-render from existing scaling.csv only

For each N the driver runs `bench.exe N` (the ticks side + the shared out.csv)
then `bench_py.py` (parquet / feather / bi5 / csv), reads the per-N tidy
bench/results.csv those produce, and accumulates every row — already tagged with
its N — into bench/scaling.csv. It then renders bench/scaling.png: a 2x2 panel of
size + write/insert/read, each a multi-line chart (one line per format) against
N on a log x-axis. The full single-N run measures an insert *curve* over batch
size and *two* read modes; the scaling view draws both endpoints of each so it
tells the same story the comparison chart does — insert at the 1k (solid) and
100k (dashed) batch, read in scan (solid) and materialize (dashed) mode, the
styles noted in the panel titles. Collapsing to a single point per N instead
lands on ticks' two weakest sub-points (100k insert, where feather catches it,
and materialize), underselling it. The ticks lines are bold/red.

`bench.exe` must already be built (see bench/README.md); the sweep shells out to
it once per N. The heavy `run` mode is separate from `plot` so the chart can be
re-rendered without re-timing.
"""
import csv, os, subprocess, sys, time
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BENCH_EXE = os.path.join(HERE, "bench.exe")
RESULTS = os.path.join(HERE, "results.csv")
SCALING = os.path.join(HERE, "scaling.csv")
SCALING_PNG = os.path.join(HERE, "scaling.png")

FIELDS = ["format", "n", "metric", "variant", "value"]

# Geometric sweep: each step ~2-2.5x so the log x-axis is evenly spaced.
DEFAULT_NS = [100_000, 250_000, 500_000, 1_000_000, 2_500_000, 5_000_000, 10_000_000]

# Sub-points drawn per N for the insert and read panels. The full single-N run
# measures an insert *curve* over batch size and *two* read modes; rather than
# collapse each to one point (which lands on ticks' two weakest sub-points — at
# the 100k insert batch feather catches ticks, and materialize is ticks' slower
# read mode), the scaling view draws both endpoints so it tells the same story
# the comparison chart does. Solid = ticks' stronger sub-point, dashed = the
# weaker one, both real and measured.
INSERT_PLOT_BATCHES = [("1000", "1k", "solid"), ("100000", "100k", "dashed")]
READ_PLOT_MODES = [("scan", "scan", "solid"), ("materialize", "materialize", "dashed")]

ORDER = ["ticks zstd", "ticks none", "parquet zstd", "parquet snappy",
         "feather zstd", "bi5", "csv"]
HILITE = {"ticks zstd", "ticks none"}
STYLE = {  # (color, linewidth, marker)
    "ticks zstd":     ("#d62728", 2.6, "o"),
    "ticks none":     ("#ff7f0e", 2.6, "s"),
    "parquet zstd":   ("#1f77b4", 1.6, "^"),
    "parquet snappy": ("#17becf", 1.6, "v"),
    "feather zstd":   ("#2ca02c", 1.6, "D"),
    "bi5":            ("#9467bd", 1.6, "P"),
    "csv":            ("#7f7f7f", 1.6, "x"),
}


def sweep(ns):
    """Run bench.exe + bench_py.py for each N and accumulate into scaling.csv.

    Logs progress per N and per sub-step (with elapsed seconds), and rewrites
    scaling.csv after every N — so the file always reflects completed points and
    a hang is obvious from the last line printed vs. the last N written.
    """
    accumulated = []
    t_all = time.perf_counter()
    for idx, N in enumerate(ns, 1):
        t_n = time.perf_counter()
        print(f"[{idx}/{len(ns)}] N={N:,}  starting  (elapsed {time.perf_counter()-t_all:.0f}s)",
              flush=True)

        t0 = time.perf_counter()
        subprocess.run([BENCH_EXE, str(N)], cwd=ROOT, check=True)
        print(f"[{idx}/{len(ns)}] N={N:,}  ticks side done in {time.perf_counter()-t0:.1f}s",
              flush=True)

        t0 = time.perf_counter()
        subprocess.run([sys.executable, os.path.join(HERE, "bench_py.py")],
                       cwd=ROOT, check=True)
        print(f"[{idx}/{len(ns)}] N={N:,}  python side done in {time.perf_counter()-t0:.1f}s",
              flush=True)

        with open(RESULTS, newline="") as f:
            for r in csv.DictReader(f):
                accumulated.append({k: r[k] for k in FIELDS})

        # Persist after every N so partial progress survives an interruption.
        with open(SCALING, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=FIELDS)
            w.writeheader()
            w.writerows(accumulated)
        print(f"[{idx}/{len(ns)}] N={N:,}  COMPLETE in {time.perf_counter()-t_n:.1f}s  "
              f"-> {SCALING} now has {len(accumulated)} rows", flush=True)

    print(f"sweep done in {time.perf_counter()-t_all:.0f}s; wrote {SCALING} "
          f"({len(accumulated)} rows)", flush=True)


def load():
    """Read scaling.csv into {format: {n: {(metric, variant): value}}}."""
    series = {}
    with open(SCALING, newline="") as f:
        for r in csv.DictReader(f):
            fmt = series.setdefault(r["format"], {})
            fmt.setdefault(int(r["n"]), {})[(r["metric"], r["variant"])] = float(r["value"])
    return series


def plot():
    series = load()
    fmts = [f for f in ORDER if f in series] + [f for f in series if f not in ORDER]

    # Each panel draws one or more sub-point series. A series is
    # (linestyle, point->y, sub-label); size/write have a single solid series,
    # while insert and read draw both endpoints (see INSERT_PLOT_BATCHES /
    # READ_PLOT_MODES) so the panel shows ticks' strong and weak sub-points
    # rather than only the weak one. `sub` is None for single-series panels and
    # names the sub-point otherwise (used in the per-panel linestyle legend).
    # panel -> (title, unit, [series...], log_y).
    panels = [
        ("Size  (bytes / tick — lower is better)", "bytes/tick",
         [("solid", lambda n, p: p[("size", "")] / n, None)], False),
        ("Write throughput  (bulk, M ticks/s — higher is better)", "M ticks/s",
         [("solid", lambda n, p: n / p[("write", "")] / 1e6, None)], True),
        ("Insert throughput  (M ticks/s — solid 1k batch, dashed 100k)", "M ticks/s",
         [(ls, (lambda b: lambda n, p: n / p[("insert", b)] / 1e6)(batch), lbl)
          for batch, lbl, ls in INSERT_PLOT_BATCHES], True),
        ("Read throughput  (M ticks/s — solid scan, dashed materialize)", "M ticks/s",
         [(ls, (lambda m: lambda n, p: n / p[("read", m)] / 1e6)(mode), lbl)
          for mode, lbl, ls in READ_PLOT_MODES], True),
    ]

    fig, axes = plt.subplots(2, 2, figsize=(13, 8))
    fig.suptitle(".ticks vs Parquet / Feather / bi5 / CSV  —  scaling with row count",
                 fontsize=14, fontweight="bold")

    for ax, (title, unit, series_specs, log_y) in zip(axes.flat, panels):
        for linestyle, yof, _sub in series_specs:
            for fmt in fmts:
                ns = sorted(series[fmt])
                xs, ys = [], []
                for n in ns:
                    try:
                        ys.append(yof(n, series[fmt][n]))
                        xs.append(n)
                    except KeyError:
                        pass  # metric absent for this format/N
                if not xs:
                    continue
                color, lw, marker = STYLE.get(fmt, ("#333333", 1.6, "o"))
                # Label only the solid series so the shared figure legend carries
                # one entry per format, not one per (format, sub-point).
                label = fmt if linestyle == "solid" else "_nolegend_"
                ax.plot(xs, ys, marker=marker, color=color, linewidth=lw,
                        linestyle=linestyle, markersize=5, label=label,
                        zorder=3 if fmt in HILITE else 2)
        ax.set_title(title, fontsize=10)
        ax.set_ylabel(unit, fontsize=9)
        ax.set_xlabel("rows (N)", fontsize=9)
        ax.set_xscale("log")
        if log_y:
            ax.set_yscale("log")
        ax.grid(which="both", alpha=0.3)
        ax.tick_params(labelsize=8)

        # Per-panel legend distinguishing the sub-point line styles (insert/read).
        subs = [(ls, sub) for ls, _yof, sub in series_specs if sub]
        if subs:
            proxies = [plt.Line2D([], [], color="#333333", linestyle=ls, label=sub)
                       for ls, sub in subs]
            ax.legend(handles=proxies, fontsize=7, loc="best",
                      frameon=True, framealpha=0.6)

    # one shared legend below the panels
    handles, labels = axes.flat[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=len(labels),
               fontsize=9, frameon=False, bbox_to_anchor=(0.5, -0.01))
    fig.tight_layout(rect=[0, 0.04, 1, 0.96])
    fig.savefig(SCALING_PNG, dpi=120, bbox_inches="tight")
    print(f"wrote {SCALING_PNG}")


def parse_ns(args):
    ns = []
    for a in args:
        v = float(a)  # accepts 1e6 / 2.5e6 style
        if v < 1:
            raise SystemExit(f"row count must be >= 1, got {a!r}")
        ns.append(int(round(v)))
    return ns


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "plot":
        plot()  # render only, no benchmarking
    else:
        if not os.path.exists(BENCH_EXE):
            raise SystemExit(f"{BENCH_EXE} not found — build it first (see bench/README.md)")
        sweep(parse_ns(args) if args else DEFAULT_NS)
        plot()
