"""Draws the validation charts for the README from beaglex_validate's CSV output.

Usage: python scripts/plot_validation.py [input_dir] [output_dir]
Defaults: out/validation -> docs/img
"""

import csv
import math
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

# Reference palette, light mode: categorical slots 1-2 (validated as a pair),
# chart chrome and ink.
PRICE = "#2a78d6"     # slot 1: simulated price
CONTROL = "#eb6834"   # slot 2: control (fundamental value)
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
AXIS = "#c3c2b7"

plt.rcParams.update({
    "font.family": ["Segoe UI", "DejaVu Sans", "sans-serif"],
    "font.size": 10,
    "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE,
    "axes.edgecolor": AXIS,
    "axes.labelcolor": INK_2,
    "axes.titlecolor": INK,
    "axes.titlesize": 12,
    "axes.titleweight": "semibold",
    "axes.titlelocation": "left",
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.grid": True,
    "axes.axisbelow": True,
    "grid.color": GRID,
    "grid.linewidth": 0.8,
    "xtick.color": MUTED,
    "ytick.color": MUTED,
    "xtick.labelcolor": INK_2,
    "ytick.labelcolor": INK_2,
    "legend.frameon": False,
    "legend.labelcolor": INK_2,
    "lines.linewidth": 2,
    "savefig.dpi": 160,
    "savefig.bbox": "tight",
    "savefig.facecolor": SURFACE,
})


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def subtitle(ax, text):
    ax.text(0, 1.02, text, transform=ax.transAxes, color=INK_2, fontsize=9, va="bottom")


def plot_example_day(rows, out):
    hours = [int(r["time_s"]) / 3600 for r in rows]
    price = [float(r["price"]) / 100 for r in rows]
    value = [float(r["fundamental"]) / 100 for r in rows]

    fig, ax = plt.subplots(figsize=(9, 4))
    ax.plot(hours, value, color=CONTROL, label="True value (hidden)")
    ax.plot(hours, price, color=PRICE, label="Market price", linewidth=1.2)
    ax.set_title("One simulated trading day", pad=22)
    subtitle(ax, "Price emerges from 152 trading agents; only value traders can estimate the true value")
    ax.set_xlabel("Hours since open")
    ax.set_ylabel("Price ($)")
    ax.set_xlim(0, hours[-1])
    ax.yaxis.set_major_formatter(matplotlib.ticker.StrMethodFormatter("{x:.2f}"))
    # Direct labels at the right edge, the higher line's label on top, plus a legend.
    price_on_top = price[-1] >= value[-1]
    for text, y, above in (("Market price", price[-1], price_on_top), ("True value", value[-1], not price_on_top)):
        ax.annotate(text, (hours[-1], y), xytext=(6, 6 if above else -12), textcoords="offset points",
                    color=INK_2, fontsize=9)
    ax.legend(loc="upper left")
    fig.savefig(out)
    plt.close(fig)


def plot_return_distribution(rows, out):
    def standardised(xs):
        m = sum(xs) / len(xs)
        sd = math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))
        return [(x - m) / sd for x in xs]

    price = standardised([float(r["price"]) for r in rows])
    control = standardised([float(r["control"]) for r in rows])

    edges = [-8 + i * 0.5 for i in range(33)]
    centres = [(a + b) / 2 for a, b in zip(edges, edges[1:])]

    def density(xs):
        counts = [0] * (len(edges) - 1)
        for x in xs:
            if edges[0] <= x < edges[-1]:
                counts[int((x - edges[0]) / 0.5)] += 1
        return [c / (len(xs) * 0.5) if c else float("nan") for c in counts]

    fig, ax = plt.subplots(figsize=(9, 4))
    normal = [math.exp(-c * c / 2) / math.sqrt(2 * math.pi) for c in centres]
    ax.plot(centres, normal, color=MUTED, linewidth=1.2, linestyle="--", label="Normal distribution")
    ax.plot(centres, density(control), color=CONTROL, marker="o", markersize=4, label="Control (true value)")
    ax.plot(centres, density(price), color=PRICE, marker="o", markersize=4, label="Market price")
    ax.set_yscale("log")
    ax.set_ylim(1e-5, 1)
    ax.set_xlim(-8, 8)
    ax.set_title("Fat tails: big moves are far more common than a bell curve predicts", pad=22)
    subtitle(ax, f"Distribution of {len(price):,} one-minute returns over 50 out-of-sample days (log scale)")
    ax.set_xlabel("One-minute return (standard deviations)")
    ax.set_ylabel("Density")
    ax.legend(loc="upper right")
    fig.savefig(out)
    plt.close(fig)


def plot_volatility_clustering(rows, out):
    lags = [int(r["lag_min"]) for r in rows]
    price = [float(r["price_abs_acf"]) for r in rows]
    control = [float(r["control_abs_acf"]) for r in rows]
    band = float(rows[0]["band"])

    fig, ax = plt.subplots(figsize=(9, 4))
    ax.axhspan(-band, band, color=GRID, alpha=0.7, linewidth=0, label="95% band if no clustering")
    ax.axhline(0, color=AXIS, linewidth=1)
    ax.plot(lags, control, color=CONTROL, marker="o", markersize=4, label="Control (true value)")
    ax.plot(lags, price, color=PRICE, marker="o", markersize=4, label="Market price")
    ax.set_title("Volatility clustering: turbulent minutes are followed by turbulent minutes", pad=22)
    subtitle(ax, "Autocorrelation of |one-minute returns|; the control is a random walk, so it shows none")
    ax.set_xlabel("Lag (minutes)")
    ax.set_ylabel("Autocorrelation of |return|")
    ax.set_xlim(0.5, lags[-1] + 0.5)
    ax.annotate("Market price", (lags[2], price[2]), xytext=(8, 8), textcoords="offset points", color=INK_2, fontsize=9)
    ax.legend(loc="upper right")
    fig.savefig(out)
    plt.close(fig)


def main():
    src = Path(sys.argv[1] if len(sys.argv) > 1 else "out/validation")
    dst = Path(sys.argv[2] if len(sys.argv) > 2 else "docs/img")
    dst.mkdir(parents=True, exist_ok=True)

    plot_example_day(read_csv(src / "example_day.csv"), dst / "example_day.png")
    plot_return_distribution(read_csv(src / "returns.csv"), dst / "fat_tails.png")
    plot_volatility_clustering(read_csv(src / "acf.csv"), dst / "volatility_clustering.png")
    print(f"Wrote example_day.png, fat_tails.png, volatility_clustering.png to {dst}")


if __name__ == "__main__":
    main()
