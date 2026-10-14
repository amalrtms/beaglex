// The evolution view: replays recorded runs of the genetic algorithm
// (apps/export_evolution.cpp) generation by generation, and explains each
// generation's best strategy in plain words.

import data from "./data/evolution.json";

interface Generation {
  best: number;
  mean: number;
  median: number;
  mix: number[];
  spread: number;
  vol: number;
  share: number;
  bestGenome: number[];
  bestType: string;
}

interface Run {
  seed: number;
  generations: Generation[];
  holdout: {
    days: number;
    population: number;
    initialPositive: number;
    evolvedPositive: number;
    initialMeanFitness: number;
    evolvedMeanFitness: number;
    initialSpread: number;
    evolvedSpread: number;
  };
}

const RUNS = data.runs as Run[];
const GENES = data.genes as { name: string; lo: number; hi: number }[];
const ARCHETYPES = data.archetypes as string[];

const TYPES: Record<string, { label: string; color: string; blurb: string }> = {
  market_maker: { label: "Market maker", color: "--accent", blurb: "quotes both sides and earns the gap" },
  trend_follower: { label: "Trend follower", color: "--down", blurb: "buys what's rising" },
  mean_reverter: { label: "Mean reverter", color: "--value", blurb: "bets moves snap back" },
  flow_follower: { label: "Flow follower", color: "--up", blurb: "follows order-book pressure" },
  flow_fader: { label: "Flow fader", color: "--muted", blurb: "leans against order-book pressure" },
};

const $ = (id: string) => document.getElementById(id)!;

let run = RUNS[0];
let gen = 0;
let timer: number | undefined;
let onRelease: (genes: number[], label: string) => void = () => {};

// ---- plain-language genes -----------------------------------------------------

const seconds = (log10: number) => {
  const s = 10 ** log10;
  return s < 1.5 ? "every second" : s < 90 ? `every ${Math.round(s)} seconds` : `every ${(s / 60).toFixed(1)} minutes`;
};
const signed = (v: number, pos: string, neg: string, none: string) => (v > 0.2 ? pos : v < -0.2 ? neg : none);

const GENE_TEXT: { label: string; value: (v: number) => string; meaning: (v: number) => string; signed?: boolean }[] = [
  { label: "Trend following", signed: true, value: (v) => v.toFixed(2), meaning: (v) => signed(v, "buys rising prices", "bets against trends", "ignores long trends") },
  { label: "Short-term moves", signed: true, value: (v) => v.toFixed(2), meaning: (v) => signed(v, "goes with the last minute's move", "bets the last minute's move reverses", "ignores short moves") },
  { label: "Order-book pressure", signed: true, value: (v) => v.toFixed(2), meaning: (v) => signed(v, "follows the side with more orders waiting", "leans against the busier side", "ignores the order book") },
  { label: "Own inventory", signed: true, value: (v) => v.toFixed(2), meaning: (v) => signed(v, "piles further into its position", "sheds shares when it holds too many", "ignores its own position") },
  { label: "Conviction needed", value: (v) => v.toFixed(2), meaning: () => "how strong a signal must be before trading" },
  { label: "Aggressiveness", value: (v) => `${Math.round(v * 100)}%`, meaning: () => "of orders cross the spread to fill instantly" },
  { label: "Trade size", value: (v) => `${Math.round(v) * 100} shares`, meaning: () => "per order" },
  { label: "Reaction speed", value: (v) => (10 ** v < 90 ? `${Math.round(10 ** v)} s` : `${(10 ** v / 60).toFixed(1)} min`), meaning: (v) => `decides ${seconds(v)}` },
  { label: "Mode", value: (v) => (v > 0.5 ? "quotes" : "directional"), meaning: (v) => (v > 0.5 ? "keeps a buy and a sell order out at all times" : "buys or sells when its signal says so") },
  { label: "Quote width", value: (v) => `${v.toFixed(1)}¢`, meaning: () => "either side of its price, in market-making mode" },
];

function summary(g: number[], type: string): string {
  const speed = seconds(g[7]);
  if (type === "market_maker") {
    const inv = g[3] < -0.2 ? ", and leans hard against its inventory so it never holds too much" : "";
    return `Keeps a bid and an ask about ${g[9].toFixed(1)}¢ either side of the price, moves them ${speed}${inv}.`;
  }
  return `A ${TYPES[type]?.label.toLowerCase() ?? type} that trades about ${Math.round(g[6]) * 100} shares when its signal passes ${g[4].toFixed(2)}, deciding ${speed}.`;
}

// ---- charts -------------------------------------------------------------------

const W = 800;
const PAD = { l: 52, r: 12, t: 10, b: 26 };

function frame(h: number, yTicks: number[], yFmt: (v: number) => string, y: (v: number) => number) {
  const n = run.generations.length;
  const x = (i: number) => PAD.l + (i / (n - 1)) * (W - PAD.l - PAD.r);
  let s = "";
  for (const t of yTicks) {
    s += `<line x1="${PAD.l}" x2="${W - PAD.r}" y1="${y(t)}" y2="${y(t)}" style="stroke:var(--grid)"/>`;
    s += `<text x="${PAD.l - 8}" y="${y(t) + 4}" text-anchor="end" font-size="11" style="fill:var(--muted)">${yFmt(t)}</text>`;
  }
  for (let i = 0; i < n; i += 20) {
    s += `<text x="${x(i)}" y="${h - 6}" text-anchor="middle" font-size="11" style="fill:var(--muted)">${i + 1}</text>`;
  }
  s += `<text x="${W - PAD.r}" y="${h - 6}" text-anchor="end" font-size="11" style="fill:var(--muted)">generation</text>`;
  return { s, x };
}

function cursor(h: number, x: (i: number) => number) {
  return `<line x1="${x(gen)}" x2="${x(gen)}" y1="${PAD.t}" y2="${h - PAD.b}" style="stroke:var(--text)" stroke-width="1.5" stroke-dasharray="3 3"/>`;
}

function mixChart() {
  const h = 200;
  const gens = run.generations;
  const total = gens[0].mix.reduce((a, b) => a + b, 0);
  const y = (v: number) => PAD.t + (1 - v / total) * (h - PAD.t - PAD.b);
  const { s, x } = frame(h, [0, total / 2, total], (v) => String(v), y);
  let areas = "";
  const base = gens.map(() => 0);
  ARCHETYPES.forEach((type, a) => {
    const top = gens.map((g, i) => base[i] + g.mix[a]);
    const pts = top.map((v, i) => `${x(i)},${y(v)}`).concat(base.map((v, i) => `${x(i)},${y(v)}`).reverse());
    areas += `<polygon points="${pts.join(" ")}" style="fill:var(${TYPES[type].color})" fill-opacity="0.85"/>`;
    top.forEach((v, i) => (base[i] = v));
  });
  return `<svg viewBox="0 0 ${W} ${h}" role="img" aria-label="Strategy mix by generation">${s}${areas}${cursor(h, x)}</svg>`;
}

function lineChart(h: number, series: { values: number[]; color: string; width: number }[], yFmt: (v: number) => string, zero = false) {
  const all = series.flatMap((s) => s.values);
  let lo = Math.min(...all);
  let hi = Math.max(...all);
  if (zero) lo = Math.min(lo, 0);
  const padY = (hi - lo) * 0.08;
  lo -= padY;
  hi += padY;
  const y = (v: number) => PAD.t + (1 - (v - lo) / (hi - lo)) * (h - PAD.t - PAD.b);
  const step = niceStep((hi - lo) / 4);
  const ticks: number[] = [];
  for (let t = Math.ceil(lo / step) * step; t <= hi; t += step) ticks.push(t);
  const { s, x } = frame(h, ticks, yFmt, y);
  let lines = "";
  for (const sr of series) {
    lines += `<polyline fill="none" stroke-width="${sr.width}" stroke-linejoin="round" style="stroke:var(${sr.color})" points="${sr.values.map((v, i) => `${x(i)},${y(v)}`).join(" ")}"/>`;
  }
  const dots = series
    .map((sr) => `<circle cx="${x(gen)}" cy="${y(sr.values[gen])}" r="4" style="fill:var(${sr.color});stroke:var(--panel)" stroke-width="2"/>`)
    .join("");
  return `<svg viewBox="0 0 ${W} ${h}" role="img">${s}${lines}${cursor(h, x)}${dots}</svg>`;
}

function niceStep(raw: number) {
  const p = 10 ** Math.floor(Math.log10(raw));
  const m = raw / p;
  return (m < 1.5 ? 1 : m < 3 ? 2 : m < 7 ? 5 : 10) * p;
}

const dollars = (v: number) => (Math.abs(v) >= 1000 ? `$${(v / 1000).toFixed(v % 1000 === 0 ? 0 : 1)}k` : `$${Math.round(v)}`);

function renderCharts() {
  $("chart-mix").innerHTML = mixChart();
  $("mix-key").innerHTML = ARCHETYPES.map(
    (t) => `<span><i style="background:var(${TYPES[t].color})"></i>${TYPES[t].label} <small>· ${TYPES[t].blurb}</small></span>`,
  ).join("");
  $("chart-fitness").innerHTML =
    lineChart(
      180,
      [
        { values: run.generations.map((g) => g.mean), color: "--muted", width: 1.5 },
        { values: run.generations.map((g) => g.best), color: "--accent", width: 2.5 },
      ],
      dollars,
      true,
    ) +
    `<div class="key"><span><i style="background:var(--accent)"></i>Best strategy</span><span><i style="background:var(--muted)"></i>Average strategy</span></div>`;
  $("chart-spread").innerHTML = lineChart(160, [{ values: run.generations.map((g) => g.spread), color: "--value", width: 2.5 }], (v) => `${v.toFixed(1)}¢`);
}

// ---- panels -------------------------------------------------------------------

function renderHoldout() {
  const h = run.holdout;
  const tile = (label: string, before: string, after: string) =>
    `<div class="stat"><span>${label}</span><strong>${before} → ${after}</strong></div>`;
  $("holdout").innerHTML =
    `<div class="run-pick segmented">${RUNS.map((r) => `<button data-seed="${r.seed}" class="${r === run ? "active" : ""}">Run ${r.seed}</button>`).join("")}</div>` +
    tile("Profitable on unseen days", `${h.initialPositive}`, `${h.evolvedPositive} <small>of ${h.population}</small>`) +
    tile("Market spread", `${h.initialSpread.toFixed(2)}¢`, `${h.evolvedSpread.toFixed(2)}¢`) +
    `<p class="muted" style="grid-column:1/-1;margin:0">Random starting strategies vs the final evolved ones, tested on ${h.days} market days never used during evolution.</p>`;
}

function renderChampion() {
  const g = run.generations[gen];
  const t = TYPES[g.bestType] ?? { label: g.bestType, color: "--muted", blurb: "" };
  const genes = g.bestGenome
    .map((v, i) => {
      const spec = GENES[i];
      const txt = GENE_TEXT[i];
      const pos = ((v - spec.lo) / (spec.hi - spec.lo)) * 100;
      // Quoting strategies never consult threshold or aggression; directional ones never consult quote width.
      const quoting = g.bestGenome[8] > 0.5;
      const unused = quoting ? i === 4 || i === 5 : i === 9;
      const meaning = unused
        ? `unused by this strategy, so evolution can't see it and it drifts at random`
        : txt.meaning(v);
      return (
        `<div class="gene${unused ? " unused" : ""}"><div class="top"><b>${txt.label}</b><span>${txt.value(v)}</span></div>` +
        `<div class="track${txt.signed ? " signed" : ""}"><i style="left:${pos.toFixed(1)}%"></i></div>` +
        `<small>${meaning}</small></div>`
      );
    })
    .join("");
  $("champion").innerHTML =
    `<div class="panel-head"><h2>Best strategy</h2><span class="muted">generation ${gen + 1}, run ${run.seed}</span></div>` +
    `<span class="type" style="background:color-mix(in srgb, var(${t.color}) 20%, transparent);color:var(${t.color})">${t.label}</span>` +
    `<div class="profit">${dollars(g.best)}<span class="muted"> per day</span></div>` +
    `<p class="summary">${summary(g.bestGenome, g.bestType)}</p>` +
    `<div class="genes">${genes}</div>` +
    `<button class="btn primary wide" id="release">Release 5 copies into the live market</button>`;
  $("release").addEventListener("click", () => onRelease(g.bestGenome, `Gen ${gen + 1} champions`));
}

function setGeneration(i: number) {
  gen = Math.max(0, Math.min(run.generations.length - 1, i));
  ($("evo-gen") as HTMLInputElement).value = String(gen);
  $("evo-gen-label").textContent = `Generation ${gen + 1}`;
  renderCharts();
  renderChampion();
}

function setPlaying(p: boolean) {
  if (timer !== undefined) clearInterval(timer);
  timer = undefined;
  $("evo-play").textContent = p ? "Pause" : "Play";
  if (!p) return;
  if (gen >= run.generations.length - 1) setGeneration(0);
  timer = window.setInterval(() => {
    if (gen >= run.generations.length - 1) setPlaying(false);
    else setGeneration(gen + 1);
  }, 160);
}

// ---- public -------------------------------------------------------------------

export function initEvolution(release: (genes: number[], label: string) => void) {
  onRelease = release;
  const slider = $("evo-gen") as HTMLInputElement;
  slider.max = String(run.generations.length - 1);
  slider.addEventListener("input", () => {
    setPlaying(false);
    setGeneration(Number(slider.value));
  });
  $("evo-play").addEventListener("click", () => setPlaying(timer === undefined));
  $("holdout").addEventListener("click", (e) => {
    const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-seed]");
    if (!btn) return;
    run = RUNS.find((r) => r.seed === Number(btn.dataset.seed)) ?? run;
    renderHoldout();
    setGeneration(gen);
  });
  // Click or drag on any chart to jump to that generation.
  document.querySelectorAll<HTMLElement>(".evo-chart").forEach((el) => {
    const pick = (e: PointerEvent) => {
      const svg = el.querySelector("svg");
      if (!svg) return;
      const r = svg.getBoundingClientRect();
      const fx = ((e.clientX - r.left) / r.width) * W;
      const t = (fx - PAD.l) / (W - PAD.l - PAD.r);
      setPlaying(false);
      setGeneration(Math.round(t * (run.generations.length - 1)));
    };
    el.addEventListener("pointerdown", (e) => {
      if (!(e.target as Element).closest("svg")) return;
      pick(e);
      const move = (ev: PointerEvent) => pick(ev);
      window.addEventListener("pointermove", move);
      window.addEventListener("pointerup", () => window.removeEventListener("pointermove", move), { once: true });
    });
  });
  renderHoldout();
  setGeneration(0);
}

export function pauseEvolution() {
  setPlaying(false);
}
