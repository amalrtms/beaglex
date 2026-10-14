"""Draws the Phase 6 experiment charts from beaglex_experiment and beaglex_evolve output.

Usage: python scripts/plot_experiments.py [experiments_dir] [predators_dir] [output_dir]
Defaults: out/experiments out/predators docs/img
"""

import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from plot_validation import AXIS, INK_2, MUTED, SURFACE, subtitle  # shared style  # noqa: E402

# Reference palette, light mode, categorical slots in fixed order.
SLOT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"]
CALM_BAND = "#e1e0d9"


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def calm_reference(ax, value, label="calm market (no algos)"):
    # Labelled at the right edge, just below the line, clear of the data.
    ax.axhline(value, color=MUTED, linewidth=1, linestyle="--")
    ax.text(ax.get_xlim()[1], value * 0.92, label + "  ", color=INK_2, fontsize=9, va="top", ha="right")


def plot_algo_share(rows, out):
    fig, ax = plt.subplots(figsize=(9, 4.2))
    calm = None
    for group, label, colour in (("momentum", "Momentum algos (take liquidity)", SLOT[1]),
                                 ("market_maker", "Market-making algos (provide liquidity)", SLOT[0])):
        pts = [r for r in rows if r["group"] == group]
        n = [int(r["condition"].split()[0]) for r in pts]
        vol = [float(r["volatility_bp"]) for r in pts]
        ci = [float(r["volatility_ci"]) for r in pts]
        calm = vol[0]
        ax.errorbar(n, vol, yerr=ci, color=colour, marker="o", markersize=5, capsize=3, label=label)
        ax.annotate(label.split(" (")[0], (n[-1], vol[-1]), xytext=(-6, 8), textcoords="offset points",
                    color=INK_2, fontsize=9, ha="right")
    ax.set_yscale("log")
    ax.set_xlim(-2, 82)
    calm_reference(ax, calm)
    ax.set_title("A few identical trend-chasing algorithms are enough to destabilise the market", pad=22)
    subtitle(ax, "One-minute volatility (log scale), mean of 20 paired days with 95% intervals; "
                 "momentum algos are under 2.5% of volume")
    ax.set_xlabel("Number of algorithms added")
    ax.set_ylabel("Volatility (bp per minute)")
    ax.legend(loc="center right")
    fig.savefig(out)
    plt.close(fig)


def plot_herding(rows, out):
    groups = [("identical", "Identical"), ("diverse_same_speed", "Diverse signals, same speed"),
              ("diverse_varied_speed", "Diverse signals and speeds")]
    sizes = [10, 20, 40]
    width = 0.26
    fig, ax = plt.subplots(figsize=(9, 4.2))
    for k, (group, label) in enumerate(groups):
        vals, cis = [], []
        for n in sizes:
            r = next(r for r in rows if r["group"] == group and r["condition"].startswith(f"{n} "))
            vals.append(float(r["volatility_bp"]))
            cis.append(float(r["volatility_ci"]))
        xs = [i + (k - 1) * width for i in range(len(sizes))]
        bars = ax.bar(xs, vals, width=width - 0.03, color=SLOT[k], label=label, yerr=cis, capsize=3,
                      error_kw={"ecolor": INK_2, "elinewidth": 1})
        for x, v in zip(xs, vals):
            ax.text(x, v * 1.25, f"{v:.0f}", ha="center", va="bottom", color=INK_2, fontsize=8)
    ax.set_yscale("log")
    ax.set_ylim(5, 3000)
    ax.set_xticks(range(len(sizes)), [f"{n} algos" for n in sizes])
    ax.grid(axis="x", visible=False)
    ax.set_title("Herding: identical algorithms do far more damage than diverse ones", pad=22)
    subtitle(ax, "One-minute volatility (bp, log scale) with the same number of momentum algos, "
                 "20 paired days per bar")
    ax.set_ylabel("Volatility (bp per minute)")
    ax.legend(loc="upper left")
    fig.savefig(out)
    plt.close(fig)


def plot_circuit_breakers(rows, out):
    pts = [r for r in rows if r["group"] == "algo_heavy"]
    labels = [r["condition"].replace("40 algos, ", "") for r in pts]
    vol = [float(r["volatility_bp"]) for r in pts]
    ci = [float(r["volatility_ci"]) for r in pts]
    halts = [float(r["halts_per_day"]) for r in pts]

    fig, ax = plt.subplots(figsize=(9, 3.8))
    ys = list(range(len(pts)))[::-1]
    ax.barh(ys, vol, xerr=ci, color=SLOT[0], height=0.6, capsize=3, error_kw={"ecolor": INK_2, "elinewidth": 1})
    for y, v, c, h in zip(ys, vol, ci, halts):
        note = f"{v:,.0f} bp" + (f"   ·   halted {h * 5 / 390:.0%} of the day" if h > 0 else "")
        ax.text(v + c + max(vol) * 0.02, y, note, va="center", color=INK_2, fontsize=9)
    ax.set_yticks(ys, labels)
    ax.set_xlim(0, max(vol) * 1.55)
    ax.grid(axis="y", visible=False)
    ax.set_title("Circuit breakers treat the symptom: bands alone make things worse", pad=22)
    subtitle(ax, "One-minute volatility with 40 identical momentum algos, 20 paired days per bar")
    ax.set_xlabel("Volatility (bp per minute)")
    fig.savefig(out)
    plt.close(fig)


def plot_predators(rows, out):
    gens = [int(r["generation"]) for r in rows]
    total = sum(int(rows[0][k]) for k in ("n_market_maker", "n_trend_follower", "n_mean_reverter",
                                         "n_flow_follower", "n_flow_fader"))
    reverters = [100 * int(r["n_mean_reverter"]) / total for r in rows]
    makers = [100 * int(r["n_market_maker"]) / total for r in rows]
    others = [100 - a - b for a, b in zip(reverters, makers)]
    vol = [float(r["return_sd_bp"]) for r in rows]

    fig, (top, bottom) = plt.subplots(2, 1, figsize=(9, 6), sharex=True, gridspec_kw={"height_ratios": [1, 1.1]})
    top.stackplot(gens, reverters, makers, others, colors=[SLOT[2], SLOT[0], CALM_BAND],
                  labels=["Mean reverters (predators)", "Market makers", "Other"], edgecolor=SURFACE, linewidth=1.0)
    top.set_ylim(0, 100)
    top.set_ylabel("Share of population (%)")
    top.grid(axis="x", visible=False)
    top.legend(loc="upper left", bbox_to_anchor=(1.01, 1.0), fontsize=9)
    top.set_title("Predators evolve to feed on trend-chasing algorithms, and calm the market", pad=22)
    subtitle(top, "Evolution in a market containing 20 fixed momentum algos")

    bottom.plot(gens, vol, color=SLOT[1])
    bottom.set_yscale("log")
    bottom.axhline(7.0, color=MUTED, linewidth=1, linestyle="--")
    bottom.text(gens[-1], 7.0 * 1.1, "calm market (no algos)  ", color=INK_2, fontsize=9, ha="right", va="bottom")
    bottom.set_ylabel("Volatility (bp per minute)")
    bottom.set_xlabel("Generation")
    bottom.set_xlim(gens[0], gens[-1])
    fig.savefig(out)
    plt.close(fig)


def main():
    exp = Path(sys.argv[1] if len(sys.argv) > 1 else "out/experiments")
    pred = Path(sys.argv[2] if len(sys.argv) > 2 else "out/predators")
    dst = Path(sys.argv[3] if len(sys.argv) > 3 else "docs/img")
    dst.mkdir(parents=True, exist_ok=True)
    plot_algo_share(read_csv(exp / "algo_share.csv"), dst / "exp_algo_share.png")
    plot_herding(read_csv(exp / "herding.csv"), dst / "exp_herding.png")
    plot_circuit_breakers(read_csv(exp / "circuit_breakers.csv"), dst / "exp_circuit_breakers.png")
    plot_predators(read_csv(pred / "generations.csv"), dst / "exp_predators.png")
    print(f"Wrote exp_algo_share.png, exp_herding.png, exp_circuit_breakers.png, exp_predators.png to {dst}")


if __name__ == "__main__":
    main()
