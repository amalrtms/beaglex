"""Draws the evolution charts for the README from beaglex_evolve's CSV output.

Usage: python scripts/plot_evolution.py [input_dir] [output_dir]
Defaults: out/evolution -> docs/img
"""

import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from plot_validation import INK_2, MUTED, SURFACE, subtitle  # shared style  # noqa: E402

# Reference palette categorical slots 1-5, light mode, in fixed order.
ARCHETYPES = [
    ("n_market_maker", "Market maker", "#2a78d6"),
    ("n_trend_follower", "Trend follower", "#eb6834"),
    ("n_mean_reverter", "Mean reverter", "#1baf7a"),
    ("n_flow_follower", "Order-flow follower", "#eda100"),
    ("n_flow_fader", "Order-flow fader", "#e87ba4"),
]
FITNESS_BEST = "#2a78d6"
FITNESS_MEAN = "#eb6834"


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def plot_strategy_mix(rows, out):
    gens = [int(r["generation"]) for r in rows]
    total = sum(int(rows[0][key]) for key, _, _ in ARCHETYPES)
    series = [[100 * int(r[key]) / total for r in rows] for key, _, _ in ARCHETYPES]

    fig, ax = plt.subplots(figsize=(9, 4))
    stacks = ax.stackplot(gens, *series, colors=[c for _, _, c in ARCHETYPES],
                          labels=[label for _, label, _ in ARCHETYPES],
                          edgecolor=SURFACE, linewidth=1.0)
    ax.set_xlim(gens[0], gens[-1])
    ax.set_ylim(0, 100)
    ax.set_title("Evolution discovers market making", pad=22)
    subtitle(ax, "Share of the 20 evolving strategies by archetype; all start random")
    ax.set_xlabel("Generation")
    ax.set_ylabel("Share of population (%)")
    ax.grid(axis="x", visible=False)
    # Direct label inside the band that ends up dominating.
    ax.text(gens[-1] * 0.6, 50, "Market makers", color="#ffffff", fontsize=11, fontweight="semibold", ha="center")
    ax.legend(stacks[::-1], [label for _, label, _ in ARCHETYPES][::-1], loc="center left", bbox_to_anchor=(1.01, 0.5))
    fig.savefig(out)
    plt.close(fig)


def plot_spread(rows, out):
    gens = [int(r["generation"]) for r in rows]
    spread = [float(r["avg_spread_ticks"]) for r in rows]

    fig, ax = plt.subplots(figsize=(9, 3.5))
    ax.plot(gens, spread, color=FITNESS_BEST)
    ax.axhline(1, color=MUTED, linewidth=1, linestyle="--")
    ax.text(gens[0] + 1, 0.96, "minimum possible (1 tick)", color=INK_2, fontsize=9, va="top", ha="left")
    ax.set_xlim(gens[0], gens[-1])
    ax.set_ylim(0, max(spread) * 1.15)
    ax.set_title("Competing evolved market makers drive the spread to its minimum", pad=22)
    subtitle(ax, "Average bid-ask spread of the whole market, by generation")
    ax.set_xlabel("Generation")
    ax.set_ylabel("Spread (ticks)")
    fig.savefig(out)
    plt.close(fig)


def plot_fitness(rows, out):
    gens = [int(r["generation"]) for r in rows]
    best = [float(r["best_fitness"]) for r in rows]
    mean = [float(r["mean_fitness"]) for r in rows]

    fig, ax = plt.subplots(figsize=(9, 3.5))
    ax.axhline(0, color=MUTED, linewidth=1)
    ax.plot(gens, best, color=FITNESS_BEST, label="Best strategy")
    ax.plot(gens, mean, color=FITNESS_MEAN, label="Population mean")
    ax.set_xlim(gens[0], gens[-1])
    ax.set_title("Competition erodes the winners' profits", pad=22)
    subtitle(ax, "Risk-adjusted daily profit ($), on fresh market days each generation")
    ax.set_xlabel("Generation")
    ax.set_ylabel("Fitness ($ per day)")
    ax.yaxis.set_major_formatter(matplotlib.ticker.StrMethodFormatter("{x:,.0f}"))
    ax.legend(loc="upper right")
    fig.savefig(out)
    plt.close(fig)


def main():
    src = Path(sys.argv[1] if len(sys.argv) > 1 else "out/evolution")
    dst = Path(sys.argv[2] if len(sys.argv) > 2 else "docs/img")
    dst.mkdir(parents=True, exist_ok=True)
    rows = read_csv(src / "generations.csv")
    plot_strategy_mix(rows, dst / "evolution_mix.png")
    plot_spread(rows, dst / "evolution_spread.png")
    plot_fitness(rows, dst / "evolution_fitness.png")
    print(f"Wrote evolution_mix.png, evolution_spread.png, evolution_fitness.png to {dst}")


if __name__ == "__main__":
    main()
