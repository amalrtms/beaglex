export interface Market {
  advance(seconds: number): boolean;
  time(): number;
  duration(): number;
  events(): number;
  sampleCount(): number;
  tradeCount(): number;
  halted(): boolean;
  halts(): number;
  samplesSince(from: number): Float64Array;
  tradesSince(from: number): Float64Array;
  bids(levels: number): Float64Array;
  asks(levels: number): Float64Array;
  groupStats(): { group: string; count: number; pnl: number; volume: number; position: number }[];
  addMomentumAlgos(n: number, diverse: boolean): void;
  addStrategy(genes: number[], n: number, group: string): void;
  newsShock(percent: number): void;
  setCircuitBreaker(mode: number): void;
  delete(): void;
}
export interface BeaglexModule {
  presetGenome(name: "evolved_market_maker" | "momentum_algo"): number[];
  Market: new (seed: number, marketMakers: number, noise: number, momentum: number, value: number) => Market;
}
const createBeaglex: () => Promise<BeaglexModule>;
export default createBeaglex;
