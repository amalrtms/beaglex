// Runs one simulated trading day and writes the results as CSV.
// Usage: beaglex_sim [--seed N] [--hours H] [--out DIR]

#include "beaglex/sim/population.hpp"
#include "beaglex/sim/simulation.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

using namespace beaglex;
using namespace beaglex::sim;

namespace {

std::string opt_price(const std::optional<Price>& p) {
    return p ? std::to_string(*p) : "";
}

void write_samples(const Simulation& sim, const std::filesystem::path& path) {
    std::ofstream out(path);
    out << "time_s,bid,ask,mid,last,bid_size,ask_size,fundamental,resting_orders\n";
    for (const Sample& s : sim.samples()) {
        const auto mid = s.market.mid();
        out << s.time / kSecond << ',' << opt_price(s.market.best_bid) << ',' << opt_price(s.market.best_ask) << ','
            << (mid ? std::to_string(*mid) : "") << ',' << opt_price(s.market.last_trade) << ','
            << s.market.bid_size << ',' << s.market.ask_size << ',' << s.fundamental << ',' << s.resting_orders
            << '\n';
    }
}

void write_trades(const Simulation& sim, const std::filesystem::path& path) {
    std::ofstream out(path);
    out << "time_ns,price,qty,taker_side,maker_kind,taker_kind\n";
    for (const TradeRecord& t : sim.trades()) {
        out << t.time << ',' << t.price << ',' << t.qty << ',' << to_string(t.taker_side) << ','
            << sim.agent(t.maker).kind() << ',' << sim.agent(t.taker).kind() << '\n';
    }
}

void write_agents(const Simulation& sim, double close, const std::filesystem::path& path) {
    std::ofstream out(path);
    out << "id,kind,latency_us,position,volume,fills,fees_usd,pnl_usd\n";
    for (AgentId id = 0; id < sim.agent_count(); ++id) {
        const Account& a = sim.account(id);
        out << id << ',' << sim.agent(id).kind() << ',' << sim.latency(id) / kMicrosecond << ',' << a.position << ','
            << a.volume << ',' << a.fills << ',' << to_dollars(a.fees) << ',' << to_dollars(a.mark_to_market(close))
            << '\n';
    }
}

void print_summary(const Simulation& sim, double wall_seconds) {
    const auto& samples = sim.samples();
    std::optional<double> open, close;
    double high = 0, low = 1e18, spread_sum = 0, abs_dev_bps = 0;
    std::size_t two_sided = 0;
    for (const Sample& s : samples) {
        const auto mid = s.market.mid();
        if (!mid) {
            continue;
        }
        if (!open) {
            open = mid;
        }
        close = mid;
        high = std::max(high, *mid);
        low = std::min(low, *mid);
        spread_sum += static_cast<double>(*s.market.best_ask - *s.market.best_bid);
        abs_dev_bps += std::abs(*mid / s.fundamental - 1.0) * 1e4;
        ++two_sided;
    }

    Qty volume = 0;
    for (const auto& t : sim.trades()) {
        volume += t.qty;
    }

    std::printf("Simulated %.1f hours in %.2f s wall time (%zu events, %.1fM events/s)\n\n",
                static_cast<double>(sim.config().duration) / static_cast<double>(kHour), wall_seconds,
                sim.events_processed(), static_cast<double>(sim.events_processed()) / wall_seconds / 1e6);

    if (!open) {
        std::printf("The market never had both a bid and an ask.\n");
        return;
    }
    std::printf("Market\n");
    std::printf("  price      open %s  close %s  high %s  low %s\n", format_price(std::llround(*open)).c_str(),
                format_price(std::llround(*close)).c_str(), format_price(std::llround(high)).c_str(),
                format_price(std::llround(low)).c_str());
    std::printf("  true value open %s  close %s\n", format_price(std::llround(samples.front().fundamental)).c_str(),
                format_price(std::llround(samples.back().fundamental)).c_str());
    std::printf("  trades     %zu (%lld shares)\n", sim.trades().size(), static_cast<long long>(volume));
    std::printf("  avg spread %.2f ticks, two-sided %.1f%% of the time\n", spread_sum / static_cast<double>(two_sided),
                100.0 * static_cast<double>(two_sided) / static_cast<double>(samples.size()));
    std::printf("  avg |price - true value| %.1f bp\n\n", abs_dev_bps / static_cast<double>(two_sided));

    struct KindStats {
        std::size_t count = 0;
        Qty volume = 0;
        Money pnl = 0, fees = 0;
        Qty max_abs_position = 0;
    };
    std::map<std::string, KindStats> by_kind;
    for (AgentId id = 0; id < sim.agent_count(); ++id) {
        const Account& a = sim.account(id);
        KindStats& k = by_kind[sim.agent(id).kind()];
        ++k.count;
        k.volume += a.volume;
        k.pnl += a.mark_to_market(*close);
        k.fees += a.fees;
        k.max_abs_position = std::max(k.max_abs_position, std::abs(a.position));
    }
    std::printf("%-14s %6s %12s %14s %14s %14s\n", "Agents", "count", "volume", "PnL/agent $", "fees/agent $",
                "max |position|");
    for (const auto& [kind, k] : by_kind) {
        const auto n = static_cast<double>(k.count);
        std::printf("%-14s %6zu %12lld %14.2f %14.2f %14lld\n", kind.c_str(), k.count, static_cast<long long>(k.volume),
                    to_dollars(k.pnl) / n, to_dollars(k.fees) / n, static_cast<long long>(k.max_abs_position));
    }
    std::printf("Exchange fee revenue: $%.2f\n", to_dollars(sim.exchange_revenue()));
}

} // namespace

int main(int argc, char** argv) {
    SimConfig config;
    std::filesystem::path out_dir = "out";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--seed")) {
            config.seed = std::strtoull(argv[i + 1], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--hours")) {
            config.duration = static_cast<Time>(std::atof(argv[i + 1]) * static_cast<double>(kHour));
        } else if (!std::strcmp(argv[i], "--out")) {
            out_dir = argv[i + 1];
        } else {
            std::fprintf(stderr, "usage: beaglex_sim [--seed N] [--hours H] [--out DIR]\n");
            return 2;
        }
    }

    Simulation sim(config);
    add_population(sim, PopulationConfig{});

    const auto t0 = std::chrono::steady_clock::now();
    sim.run();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    print_summary(sim, wall);

    std::filesystem::create_directories(out_dir);
    double close = static_cast<double>(config.fundamental.initial);
    for (auto it = sim.samples().rbegin(); it != sim.samples().rend(); ++it) {
        if (auto m = it->market.mid()) {
            close = *m;
            break;
        }
    }
    write_samples(sim, out_dir / "samples.csv");
    write_trades(sim, out_dir / "trades.csv");
    write_agents(sim, close, out_dir / "agents.csv");
    std::printf("\nWrote samples.csv, trades.csv, agents.csv to %s\n", out_dir.string().c_str());
    return 0;
}
