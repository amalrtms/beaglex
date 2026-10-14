import "./style.css";
import {
  createChart,
  ColorType,
  CrosshairMode,
  LineStyle,
  type IChartApi,
  type ISeriesApi,
  type SeriesMarker,
  type UTCTimestamp,
} from "lightweight-charts";
import createBeaglex, { type BeaglexModule, type Market } from "./wasm/beaglex.mjs";
import { initEvolution, pauseEvolution } from "./evolution";

// ---- constants ---------------------------------------------------------------

const OPEN = Date.UTC(2026, 0, 5, 9, 30) / 1000; // chart time axis: simulated seconds after a 09:30 open
const BOOK_LEVELS = 10;
const TAPE_ROWS = 40;
const UI_INTERVAL_MS = 120; // DOM panels refresh at this rate; the chart every frame

const GROUPS: Record<string, { label: string; note: string }> = {
  market_maker: { label: "Market makers", note: "quote both sides, earn the spread" },
  noise: { label: "Noise traders", note: "trade at random" },
  momentum: { label: "Momentum traders", note: "follow trends" },
  value: { label: "Value traders", note: "can see the true value" },
  algo_identical: { label: "Identical algos", note: "you added these" },
  algo_diverse: { label: "Diverse algos", note: "you added these" },
  evolved_mm: { label: "Evolved market makers", note: "discovered by evolution" },
};

function groupInfo(group: string) {
  if (group.startsWith("custom:")) return { label: group.slice(7), note: "evolved strategy you released" };
  return GROUPS[group] ?? { label: group, note: "" };
}

interface Mix {
  marketMakers: number;
  noise: number;
  momentum: number;
  value: number;
}
const DEFAULT_MIX: Mix = { marketMakers: 2, noise: 100, momentum: 20, value: 30 };
const MIX_SLIDERS: { key: keyof Mix; label: string; max: number }[] = [
  { key: "marketMakers", label: "Market makers", max: 10 },
  { key: "noise", label: "Noise traders", max: 300 },
  { key: "momentum", label: "Momentum traders", max: 150 },
  { key: "value", label: "Value traders", max: 150 },
];

// ---- helpers -----------------------------------------------------------------

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;

const dollars = (ticks: number) => (ticks / 100).toFixed(2);
const money = (usd: number) => {
  const sign = usd < 0 ? "−" : usd > 0 ? "+" : "";
  const abs = Math.abs(usd);
  const text = abs >= 1e6 ? `${(abs / 1e6).toFixed(2)}M` : abs >= 1e4 ? `${(abs / 1e3).toFixed(1)}k` : abs.toFixed(0);
  return `${sign}$${text}`;
};
const compact = (n: number) =>
  Math.abs(n) >= 1e6 ? `${(n / 1e6).toFixed(1)}M` : Math.abs(n) >= 1e3 ? `${(n / 1e3).toFixed(1)}k` : n.toFixed(0);
const clockText = (seconds: number) => {
  const t = 9.5 * 3600 + Math.floor(seconds);
  const hh = Math.floor(t / 3600);
  const mm = Math.floor((t % 3600) / 60);
  const ss = t % 60;
  return [hh, mm, ss].map((v) => String(v).padStart(2, "0")).join(":");
};
const css = (name: string) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

// ---- state -------------------------------------------------------------------

interface Candle {
  time: UTCTimestamp;
  open: number;
  high: number;
  low: number;
  close: number;
}

let mod: BeaglexModule;
let market: Market | null = null;
let mix: Mix = { ...DEFAULT_MIX };
let pendingMix: Mix = { ...DEFAULT_MIX };
let seed = Math.floor(Math.random() * 1e9);
let playing = true;
let finished = false;
let speed = 60;
let breakerMode = 0;

let sampleCursor = 0;
let tradeCursor = 0;
let candles: Candle[] = [];
let lastSample = { time: 0, bid: NaN, ask: NaN, last: NaN, value: NaN };
const tape: { time: number; price: number; qty: number; side: number }[] = [];
let markers: SeriesMarker<UTCTimestamp>[] = [];
let lastUi = 0;
let lastFrame = 0;
let lastValuePoint = -1;

// ---- chart -------------------------------------------------------------------

let chart: IChartApi;
let candleSeries: ISeriesApi<"Candlestick">;
let valueSeries: ISeriesApi<"Line">;

function buildChart() {
  const el = $("chart");
  chart = createChart(el, {
    autoSize: true,
    layout: { background: { type: ColorType.Solid, color: "transparent" }, fontSize: 11 },
    crosshair: { mode: CrosshairMode.Normal },
    timeScale: { timeVisible: true, secondsVisible: false, rightOffset: 8 },
    localization: { priceFormatter: (p: number) => `$${p.toFixed(2)}` },
  });
  valueSeries = chart.addLineSeries({
    lineWidth: 2,
    lineStyle: LineStyle.Dashed,
    priceLineVisible: false,
    lastValueVisible: true,
    crosshairMarkerVisible: false,
    title: "true value",
  });
  candleSeries = chart.addCandlestickSeries({ borderVisible: false });
  styleChart();
}

// The chart draws on a canvas, so it gets the page colours copied in rather than inherited.
function styleChart() {
  const grid = { color: css("--grid") };
  chart.applyOptions({
    layout: { textColor: css("--muted"), fontFamily: css("--mono") },
    grid: { vertLines: grid, horzLines: grid },
    rightPriceScale: { borderColor: css("--line") },
    timeScale: { borderColor: css("--line") },
  });
  valueSeries.applyOptions({ color: css("--value") });
  const up = css("--up");
  const down = css("--down");
  candleSeries.applyOptions({ upColor: up, downColor: down, wickUpColor: up, wickDownColor: down });
}

function resetChart() {
  candles = [];
  markers = [];
  lastValuePoint = -1;
  candleSeries.setData([]);
  candleSeries.setMarkers([]);
  valueSeries.setData([]);
  chart.timeScale().resetTimeScale();
}

function addMarker(text: string, color: string, shape: "arrowUp" | "arrowDown" | "circle") {
  if (!market) return;
  const time = (OPEN + Math.floor(market.time() / 60) * 60) as UTCTimestamp;
  markers.push({ time, position: shape === "arrowUp" ? "belowBar" : "aboveBar", color, shape, text });
  candleSeries.setMarkers(markers);
}

// ---- simulation --------------------------------------------------------------

function newDay(nextMix: Mix = mix) {
  market?.delete();
  mix = { ...nextMix };
  seed = Math.floor(Math.random() * 1e9);
  market = new mod.Market(seed, mix.marketMakers, mix.noise, mix.momentum, mix.value);
  market.setCircuitBreaker(breakerMode);
  sampleCursor = 0;
  tradeCursor = 0;
  tape.length = 0;
  finished = false;
  lastSample = { time: 0, bid: NaN, ask: NaN, last: NaN, value: NaN };
  resetChart();
  $("log").innerHTML = "";
  const agents = mix.marketMakers + mix.noise + mix.momentum + mix.value;
  document.querySelector(".tagline")!.textContent =
    `a live stock market of ${agents} trading agents, running in your browser`;
  setPlaying(true);
  renderUi();
}

function pull() {
  if (!market) return;
  const s = market.samplesSince(sampleCursor);
  for (let i = 0; i < s.length; i += 5) {
    lastSample = { time: s[i], bid: s[i + 1], ask: s[i + 2], last: s[i + 3], value: s[i + 4] };
    // One true-value point every 15 simulated seconds keeps the line light.
    const bucket = Math.floor(s[i] / 15);
    if (bucket !== lastValuePoint) {
      lastValuePoint = bucket;
      valueSeries.update({ time: (OPEN + bucket * 15) as UTCTimestamp, value: s[i + 4] / 100 });
    }
  }
  sampleCursor += s.length / 5;

  const t = market.tradesSince(tradeCursor);
  let touched: Candle | null = null;
  for (let i = 0; i < t.length; i += 4) {
    const time = t[i];
    const price = t[i + 1] / 100;
    const minute = (OPEN + Math.floor(time / 60) * 60) as UTCTimestamp;
    let c = candles[candles.length - 1];
    if (!c || c.time !== minute) {
      if (touched) candleSeries.update(touched);
      c = { time: minute, open: c ? c.close : price, high: price, low: price, close: price };
      c.high = Math.max(c.high, c.open);
      c.low = Math.min(c.low, c.open);
      candles.push(c);
    }
    c.high = Math.max(c.high, price);
    c.low = Math.min(c.low, price);
    c.close = price;
    touched = c;
  }
  if (touched) candleSeries.update(touched);
  // Only the newest trades reach the tape.
  const first = Math.max(0, t.length / 4 - TAPE_ROWS);
  for (let k = first; k < t.length / 4; k++) {
    tape.push({ time: t[k * 4], price: t[k * 4 + 1], qty: t[k * 4 + 2], side: t[k * 4 + 3] });
  }
  if (tape.length > TAPE_ROWS) tape.splice(0, tape.length - TAPE_ROWS);
  tradeCursor += t.length / 4;
}

function frame(now: number) {
  requestAnimationFrame(frame); // first, so one bad frame cannot stop the loop
  const dt = lastFrame ? Math.min(0.1, (now - lastFrame) / 1000) : 0;
  lastFrame = now;
  if (market && playing && !finished) {
    const more = market.advance(dt * speed);
    pull();
    if (!more) {
      finished = true;
      setPlaying(false);
      log("The closing bell rings. Start a new day to go again.", "neutral");
    }
  }
  if (now - lastUi > UI_INTERVAL_MS) {
    lastUi = now;
    renderUi();
  }
}

// ---- UI rendering ------------------------------------------------------------

function volatilityBp(): number {
  // Standard deviation of the last 30 one-minute log returns, in basis points.
  const closes = candles.slice(-31).map((c) => c.close);
  if (closes.length < 6) return NaN;
  const r = closes.slice(1).map((c, i) => Math.log(c / closes[i]));
  const mean = r.reduce((a, b) => a + b, 0) / r.length;
  const variance = r.reduce((a, b) => a + (b - mean) ** 2, 0) / (r.length - 1);
  return Math.sqrt(variance) * 1e4;
}

function renderStats() {
  const { bid, ask, last, value } = lastSample;
  const spread = ask - bid;
  const gap = ((last - value) / value) * 100;
  const vol = volatilityBp();
  const stat = (label: string, val: string, cls = "", hint = "") =>
    `<div class="stat ${cls}" title="${hint}"><span>${label}</span><strong>${val}</strong></div>`;
  const first = candles[0]?.open;
  const change = first ? ((last / 100 - first) / first) * 100 : NaN;
  $("stats").innerHTML = [
    stat("Last price", Number.isFinite(last) ? `$${dollars(last)}` : "—"),
    stat(
      "Day change",
      Number.isFinite(change) ? `${change >= 0 ? "+" : ""}${change.toFixed(2)}%` : "—",
      change > 0 ? "pos" : change < 0 ? "neg" : "",
    ),
    stat("Spread", Number.isFinite(spread) ? `${spread.toFixed(0)}¢` : "—", "", "Best ask minus best bid"),
    stat("True value", Number.isFinite(value) ? `$${dollars(value)}` : "—", "value", "Hidden from everyone except value traders"),
    stat(
      "Mispricing",
      Number.isFinite(gap) ? `${gap >= 0 ? "+" : ""}${gap.toFixed(2)}%` : "—",
      Math.abs(gap) > 2 ? "alert" : "",
      "How far the traded price is from the true value",
    ),
    stat(
      "Volatility",
      Number.isFinite(vol) ? `${vol.toFixed(1)} bp` : "—",
      vol > 25 ? "alert" : "",
      "Standard deviation of 1-minute returns, last 30 minutes",
    ),
    stat("Trades", compact(tradeCursor)),
    stat("Events", compact(market?.events() ?? 0), "", "Orders, cancels, fills and wake-ups processed"),
  ].join("");
}

function renderBook() {
  if (!market) return;
  const bids = market.bids(BOOK_LEVELS);
  const asks = market.asks(BOOK_LEVELS);
  let maxQty = 1;
  for (let i = 1; i < bids.length; i += 3) maxQty = Math.max(maxQty, bids[i]);
  for (let i = 1; i < asks.length; i += 3) maxQty = Math.max(maxQty, asks[i]);
  const row = (side: "bid" | "ask", price: number, qty: number, orders: number) =>
    `<div class="lvl ${side}"><i style="width:${((qty / maxQty) * 100).toFixed(1)}%"></i>` +
    `<span class="p">${dollars(price)}</span><span class="q">${qty.toLocaleString()}</span><span class="o">${orders}</span></div>`;
  const askRows: string[] = [];
  for (let i = 0; i < asks.length; i += 3) askRows.push(row("ask", asks[i], asks[i + 1], asks[i + 2]));
  const bidRows: string[] = [];
  for (let i = 0; i < bids.length; i += 3) bidRows.push(row("bid", bids[i], bids[i + 1], bids[i + 2]));
  const spread = asks.length && bids.length ? `${(asks[0] - bids[0]).toFixed(0)}¢ spread` : "one-sided book";
  $("book").innerHTML =
    `<div class="lvl head"><span class="p">Price</span><span class="q">Shares</span><span class="o">Orders</span></div>` +
    `<div class="asks">${askRows.reverse().join("")}</div>` +
    `<div class="mid">${market.halted() ? '<b class="halt">TRADING HALTED</b>' : spread}</div>` +
    `<div class="bids">${bidRows.join("")}</div>`;
}

function renderTape() {
  $("trade-count").textContent = `${tradeCursor.toLocaleString()} today`;
  $("tape").innerHTML = tape
    .slice()
    .reverse()
    .map(
      (t) =>
        `<div class="tick ${t.side > 0 ? "buy" : "sell"}"><span>${clockText(t.time)}</span>` +
        `<span>${dollars(t.price)}</span><span>${t.qty.toLocaleString()}</span></div>`,
    )
    .join("");
}

function renderGroups() {
  if (!market) return;
  const rows = market.groupStats();
  const maxAbs = Math.max(1, ...rows.map((r) => Math.abs(r.pnl)));
  $("groups").innerHTML = rows
    .sort((a, b) => b.pnl - a.pnl)
    .map((r) => {
      const g = groupInfo(r.group);
      const w = ((Math.abs(r.pnl) / maxAbs) * 50).toFixed(1);
      const bar = r.pnl >= 0 ? `left:50%;width:${w}%` : `right:50%;width:${w}%`;
      return (
        `<tr><td><b>${g.label}</b><small>${g.note}</small></td><td>${r.count}</td>` +
        `<td class="pnl"><div class="pnl-bar"><i class="${r.pnl >= 0 ? "pos" : "neg"}" style="${bar}"></i></div>` +
        `<span class="${r.pnl >= 0 ? "pos" : "neg"}">${money(r.pnl)}</span></td>` +
        `<td class="num">${compact(r.volume)}</td><td class="num">${r.position > 0 ? "+" : ""}${compact(r.position)}</td></tr>`
      );
    })
    .join("");
}

function renderClock() {
  if (!market) return;
  const t = market.time();
  $("clock").textContent = clockText(t);
  $("progress-bar").style.width = `${(t / market.duration()) * 100}%`;
  const status = $("status");
  const [text, cls] = finished ? ["CLOSED", "closed"] : market.halted() ? ["HALTED", "halted"] : ["OPEN", "open"];
  status.textContent = text;
  status.className = `status ${cls}`;
  $("book-orders").textContent = `seed ${seed}`;
}

function renderUi() {
  renderClock();
  renderStats();
  renderBook();
  renderTape();
  renderGroups();
}

function log(text: string, kind: "good" | "bad" | "neutral") {
  const el = document.createElement("div");
  el.className = `toast ${kind}`;
  el.textContent = market ? `${clockText(market.time())}  ${text}` : text;
  $("log").prepend(el);
  setTimeout(() => el.classList.add("fade"), 5000);
  setTimeout(() => el.remove(), 6000);
}

function setPlaying(p: boolean) {
  playing = p;
  $("play").textContent = playing ? "Pause" : finished ? "Closed" : "Play";
}

// ---- controls ----------------------------------------------------------------

function buildSetup() {
  $("setup").innerHTML = MIX_SLIDERS.map(
    (s) =>
      `<label class="slider"><span>${s.label}<output id="out-${s.key}">${pendingMix[s.key]}</output></span>` +
      `<input type="range" min="${s.key === "marketMakers" ? 0 : 0}" max="${s.max}" value="${pendingMix[s.key]}" data-key="${s.key}" /></label>`,
  ).join("");
  $("setup").addEventListener("input", (e) => {
    const input = e.target as HTMLInputElement;
    const key = input.dataset.key as keyof Mix;
    pendingMix[key] = Number(input.value);
    $(`out-${key}`).textContent = input.value;
  });
}

function wireControls() {
  $("play").addEventListener("click", () => {
    if (finished) return;
    setPlaying(!playing);
  });
  document.addEventListener("keydown", (e) => {
    if (e.code === "Space" && view === "market" && !(e.target instanceof HTMLInputElement || e.target instanceof HTMLSelectElement || e.target instanceof HTMLButtonElement)) {
      e.preventDefault();
      if (!finished) setPlaying(!playing);
    }
  });
  $<HTMLSelectElement>("speed").addEventListener("change", (e) => {
    speed = Number((e.target as HTMLSelectElement).value);
  });
  $("new-day").addEventListener("click", () => newDay());
  $("apply-setup").addEventListener("click", () => newDay(pendingMix));
  $<HTMLInputElement>("show-value").addEventListener("change", (e) => {
    valueSeries.applyOptions({ visible: (e.target as HTMLInputElement).checked });
  });

  document.querySelectorAll<HTMLButtonElement>("[data-algos]").forEach((btn) =>
    btn.addEventListener("click", () => {
      if (!market || finished) return;
      const n = Number(btn.dataset.algos);
      const diverse = btn.dataset.diverse === "1";
      market.addMomentumAlgos(n, diverse);
      addMarker(`+${n} ${diverse ? "diverse" : "identical"}`, diverse ? css("--accent") : css("--down"), "circle");
      log(
        diverse
          ? `${n} diverse algos join. Different signals mean they rarely all pile in at once.`
          : `${n} identical algos join. When one sees a trend, they all do.`,
        diverse ? "neutral" : "bad",
      );
    }),
  );

  document.querySelectorAll<HTMLButtonElement>("[data-news]").forEach((btn) =>
    btn.addEventListener("click", () => {
      if (!market || finished) return;
      const pct = Number(btn.dataset.news);
      market.newsShock(pct);
      addMarker(`news ${pct > 0 ? "+" : ""}${pct}%`, pct > 0 ? css("--up") : css("--down"), pct > 0 ? "arrowUp" : "arrowDown");
      log(
        `${pct > 0 ? "Good" : "Bad"} news: true value ${pct > 0 ? "up" : "down"} ${Math.abs(pct)}%. Only value traders know.`,
        pct > 0 ? "good" : "bad",
      );
    }),
  );

  $("add-evolved-mm").addEventListener("click", () => {
    if (!market || finished) return;
    market.addStrategy(mod.presetGenome("evolved_market_maker"), 5, "evolved_mm");
    addMarker("+5 evolved MMs", css("--accent"), "circle");
    log("5 evolved market makers join, quoting both sides one cent apart.", "good");
  });

  document.querySelectorAll<HTMLButtonElement>("[data-view], [data-goto]").forEach((btn) =>
    btn.addEventListener("click", () => setView((btn.dataset.view ?? btn.dataset.goto) as View)),
  );

  $("breaker").addEventListener("click", (e) => {
    const btn = (e.target as HTMLElement).closest("button");
    if (!btn || !market) return;
    breakerMode = Number(btn.dataset.mode);
    market.setCircuitBreaker(breakerMode);
    $("breaker").querySelectorAll("button").forEach((b) => b.classList.toggle("active", b === btn));
    log(["Circuit breaker off.", "5% price bands on.", "5% price bands with 5-minute halts on."][breakerMode], "neutral");
  });
}

// ---- views ---------------------------------------------------------------------

type View = "market" | "evolution";
let view: View = "market";
let resumeOnReturn = false;

function setView(next: View) {
  if (next === view) return;
  view = next;
  $("market-view").hidden = view !== "market";
  $("evolution-view").hidden = view !== "evolution";
  document.querySelectorAll<HTMLElement>("[data-market-only]").forEach((el) => (el.hidden = view !== "market"));
  document.querySelectorAll<HTMLButtonElement>("[data-view]").forEach((b) => b.classList.toggle("active", b.dataset.view === view));
  if (view === "evolution") {
    // The market waits while you're away rather than running unseen.
    resumeOnReturn = playing;
    setPlaying(false);
  } else {
    pauseEvolution();
    if (resumeOnReturn && !finished) setPlaying(true);
  }
  window.scrollTo({ top: 0 });
}

function releaseEvolved(genes: number[], label: string) {
  resumeOnReturn = true;
  setView("market");
  if (!market || finished) newDay();
  market!.addStrategy(genes, 5, `custom:${label}`);
  addMarker(`+5 ${label}`, css("--accent"), "circle");
  log(`5 copies of the ${label.toLowerCase()} join the market. Watch them in "Who's making money?"`, "good");
}

// ---- boot --------------------------------------------------------------------

async function main() {
  buildChart();
  initEvolution(releaseEvolved);
  buildSetup();
  wireControls();
  mod = await createBeaglex();
  newDay();
  requestAnimationFrame(frame);
}

main().catch((err) => {
  console.error(err);
  $("status").textContent = "ERROR";
  log(`Could not start the simulator: ${err}`, "bad");
});
