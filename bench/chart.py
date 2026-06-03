"""Render bench/results.csv (tidy long form) into a 2x2 comparison chart and a
markdown table. Run after bench.exe and bench_py.py.

  python bench/chart.py  ->  bench/comparison.png, bench/results.md

Panels: size (bar), write (bar), insert vs batch size (curve, one line per
format), and read scan-vs-materialize (grouped bars). The insert curve and the
two read modes are what make the throughput numbers honest — insert depends on
batch granularity, and a discard-while-scanning read is a different thing from
decoding into retained arrays.
"""
import csv
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ORDER = ["ticks zstd", "ticks none", "parquet zstd", "parquet snappy", "feather zstd", "bi5", "csv"]
HILITE = {"ticks zstd", "ticks none"}
COLOR = {
    "ticks zstd": "#d62728", "ticks none": "#ff7f0e",
    "parquet zstd": "#1f77b4", "parquet snappy": "#17becf",
    "feather zstd": "#2ca02c", "bi5": "#9467bd", "csv": "#7f7f7f",
}
INSERT_BATCHES = [1000, 10000, 100000]

# format -> {(metric, variant): value}
data = defaultdict(dict)
N = None
with open("bench/results.csv") as f:
    for r in csv.DictReader(f):
        data[r["format"]][(r["metric"], r["variant"])] = float(r["value"])
        N = int(r["n"])

labels = [k for k in ORDER if k in data]
colors = [COLOR.get(k, "#333333") for k in labels]


def tput(fmt, metric, variant):
    """M ticks/s for a timed metric, or None if absent."""
    s = data[fmt].get((metric, variant))
    return (N / s / 1e6) if s else None


fig, axes = plt.subplots(2, 2, figsize=(13, 8.5))
fig.suptitle(f".ticks vs Parquet / Feather / bi5 / CSV  —  {N:,} synthetic trade ticks",
             fontsize=14, fontweight="bold")
ax_size, ax_write, ax_insert, ax_read = axes.flat

# --- size (bytes/tick) ---
size_b = [data[k][("size", "")] / N for k in labels]
bars = ax_size.bar(labels, size_b, color=colors)
ax_size.set_title("Size  (bytes / tick — lower is better)", fontsize=10)
ax_size.set_ylabel("bytes/tick", fontsize=9)
for b, v in zip(bars, size_b):
    ax_size.text(b.get_x() + b.get_width() / 2, v, f"{v:.2f}", ha="center", va="bottom", fontsize=8)

# --- write throughput (bulk, best-of) ---
write_t = [tput(k, "write", "") for k in labels]
bars = ax_write.bar(labels, write_t, color=colors)
ax_write.set_title("Write throughput  (bulk, M ticks/s — higher is better)", fontsize=10)
ax_write.set_ylabel("M ticks/s", fontsize=9)
for b, v in zip(bars, write_t):
    ax_write.text(b.get_x() + b.get_width() / 2, v, f"{v:.1f}", ha="center", va="bottom", fontsize=8)

# --- insert throughput vs batch size (curve) ---
for k in labels:
    ys = [tput(k, "insert", str(bs)) for bs in INSERT_BATCHES]
    if all(y is None for y in ys):
        continue
    ax_insert.plot(INSERT_BATCHES, ys, marker="o", color=COLOR.get(k, "#333"),
                   linewidth=2.6 if k in HILITE else 1.6, markersize=5, label=k,
                   zorder=3 if k in HILITE else 2)
ax_insert.set_title("Insert throughput  (streaming, M ticks/s vs batch size)", fontsize=10)
ax_insert.set_ylabel("M ticks/s", fontsize=9)
ax_insert.set_xlabel("insert batch size (rows)", fontsize=9)
ax_insert.set_xscale("log")
ax_insert.set_yscale("log")
ax_insert.set_xticks(INSERT_BATCHES)
ax_insert.set_xticklabels(["1k", "10k", "100k"])
ax_insert.grid(which="both", alpha=0.3)
ax_insert.legend(fontsize=7, ncol=2)

# --- read: scan vs materialize (grouped bars) ---
x = range(len(labels))
scan_t = [tput(k, "read", "scan") or 0 for k in labels]
mat_t = [tput(k, "read", "materialize") or 0 for k in labels]
w = 0.4
ax_read.bar([i - w / 2 for i in x], scan_t, w, label="scan (iterate + discard)", color="#4c72b0")
ax_read.bar([i + w / 2 for i in x], mat_t, w, label="materialize (decode to arrays)", color="#dd8452")
ax_read.set_title("Read throughput  (M ticks/s — scan vs materialize)", fontsize=10)
ax_read.set_ylabel("M ticks/s", fontsize=9)
ax_read.set_xticks(list(x))
ax_read.set_xticklabels(labels)
ax_read.legend(fontsize=8)

for ax in (ax_size, ax_write, ax_read):
    ax.tick_params(axis="x", labelrotation=30, labelsize=8)
    ax.grid(axis="y", alpha=0.3)
    ax.margins(y=0.15)

fig.tight_layout(rect=[0, 0, 1, 0.96])
fig.savefig("bench/comparison.png", dpi=120)
print("wrote bench/comparison.png")

# --- markdown table ---
with open("bench/results.md", "w") as f:
    f.write(f"_{N:,} synthetic trade ticks, best-of-N timing. "
            f"Lower size is better; higher throughput is better._\n\n")
    f.write("| format | bytes/tick | size | write (M/s) | "
            "insert 1k | insert 10k | insert 100k | read scan | read mat |\n")
    f.write("|---|--:|--:|--:|--:|--:|--:|--:|--:|\n")

    def cell(v):
        return f"{v:.1f}" if v else "—"

    for k in labels:
        d = data[k]
        f.write(
            f"| {k} | {d[('size','')]/N:.2f} | {d[('size','')]/1e6:.1f} MB | "
            f"{cell(tput(k,'write',''))} | "
            f"{cell(tput(k,'insert','1000'))} | {cell(tput(k,'insert','10000'))} | "
            f"{cell(tput(k,'insert','100000'))} | "
            f"{cell(tput(k,'read','scan'))} | {cell(tput(k,'read','materialize'))} |\n"
        )
print("wrote bench/results.md")
