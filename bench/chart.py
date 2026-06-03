"""Render bench/results.csv into a 2x2 comparison chart (size + write/insert/read
throughput) and emit a markdown table. Run after bench.exe and bench_py.py.

  python bench/chart.py  ->  bench/comparison.png, bench/results.md
"""
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ORDER = ["ticks zstd", "ticks none", "parquet zstd", "parquet snappy", "feather zstd", "csv"]
HILITE = {"ticks zstd", "ticks none"}

rows = {}
with open("bench/results.csv") as f:
    for r in csv.DictReader(f):
        rows[r["format"]] = {
            "n": int(r["n"]),
            "size": int(r["size_bytes"]),
            "write_s": float(r["write_s"]),
            "insert_s": float(r["insert_s"]),
            "read_s": float(r["read_s"]),
        }

labels = [k for k in ORDER if k in rows]
n = rows[labels[0]]["n"]
colors = ["#d62728" if k in HILITE else "#1f77b4" for k in labels]

size_b   = [rows[k]["size"] / n for k in labels]                 # bytes / tick
write_t  = [n / rows[k]["write_s"] / 1e6 for k in labels]        # M ticks / s
insert_t = [n / rows[k]["insert_s"] / 1e6 for k in labels]
read_t   = [n / rows[k]["read_s"] / 1e6 for k in labels]

panels = [
    ("Size  (bytes / tick — lower is better)", size_b, "bytes/tick"),
    ("Write throughput  (M ticks/s — higher is better)", write_t, "M ticks/s"),
    ("Insert throughput  (streaming batches, M ticks/s)", insert_t, "M ticks/s"),
    ("Read throughput  (full scan, M ticks/s)", read_t, "M ticks/s"),
]

fig, axes = plt.subplots(2, 2, figsize=(13, 8))
fig.suptitle(f".ticks vs Parquet / Feather / CSV  —  {n:,} synthetic trade ticks",
             fontsize=14, fontweight="bold")
for ax, (title, vals, unit) in zip(axes.flat, panels):
    bars = ax.bar(labels, vals, color=colors)
    ax.set_title(title, fontsize=10)
    ax.set_ylabel(unit, fontsize=9)
    ax.tick_params(axis="x", labelrotation=30, labelsize=8)
    ax.grid(axis="y", alpha=0.3)
    for b, v in zip(bars, vals):
        ax.text(b.get_x() + b.get_width() / 2, v, f"{v:.2f}",
                ha="center", va="bottom", fontsize=8)
    ax.margins(y=0.15)

fig.tight_layout(rect=[0, 0, 1, 0.96])
fig.savefig("bench/comparison.png", dpi=120)
print("wrote bench/comparison.png")

# markdown table
with open("bench/results.md", "w") as f:
    f.write(f"_{n:,} synthetic trade ticks. Lower size is better; higher throughput is better._\n\n")
    f.write("| format | bytes/tick | size | write (M/s) | insert (M/s) | read (M/s) |\n")
    f.write("|---|--:|--:|--:|--:|--:|\n")
    for k in labels:
        d = rows[k]
        f.write(f"| {k} | {d['size']/n:.2f} | {d['size']/1e6:.1f} MB | "
                f"{n/d['write_s']/1e6:.1f} | {n/d['insert_s']/1e6:.1f} | "
                f"{n/d['read_s']/1e6:.1f} |\n")
print("wrote bench/results.md")
