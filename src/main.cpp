#include <pthread.h>
#include <signal.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#include "config/config.h"
#include "core/cell.h"
#include "kpi/kpi_json.h"
#include "offline/replay.h"
#include "runtime/http_server.h"
#include "runtime/slot_loop.h"
#include "runtime/udp_receiver.h"

namespace {

struct Args {
  std::string mode = "live";
  std::optional<std::string> config_path;
  std::optional<std::string> policy;
  std::string trace_path;
  uint64_t slots = 0;
  std::string out_path;
};

void usage() {
  std::fprintf(stderr,
               "usage: nr_mac_scheduler [--config FILE] [--policy NAME]\n"
               "       nr_mac_scheduler --mode offline --trace FILE --slots N --out KPI.json\n"
               "                        [--config FILE] [--policy NAME]\n");
}

std::optional<Args> parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "-h" || flag == "--help") return std::nullopt;
    if (i + 1 >= argc) {
      std::fprintf(stderr, "missing value for %s\n", flag.c_str());
      return std::nullopt;
    }
    const std::string v = argv[++i];
    if (flag == "--mode") {
      a.mode = v;
    } else if (flag == "--config") {
      a.config_path = v;
    } else if (flag == "--policy") {
      a.policy = v;
    } else if (flag == "--trace") {
      a.trace_path = v;
    } else if (flag == "--slots") {
      a.slots = std::stoull(v);
    } else if (flag == "--out") {
      a.out_path = v;
    } else {
      std::fprintf(stderr, "unknown flag %s\n", flag.c_str());
      return std::nullopt;
    }
  }
  if (a.mode != "live" && a.mode != "offline") return std::nullopt;
  if (a.mode == "offline" && (a.trace_path.empty() || a.slots == 0 || a.out_path.empty())) {
    return std::nullopt;
  }
  return a;
}

int run_offline(const nrs::Config& cfg, const Args& args) {
  const auto trace = nrs::load_trace(args.trace_path);
  const nrs::KpiReport k = nrs::replay(cfg, trace, args.slots);
  std::ofstream out(args.out_path);
  if (!out) throw std::runtime_error("cannot write " + args.out_path);
  out << nrs::to_json(k).dump(2) << '\n';
  std::fprintf(stderr, "offline: policy=%s slots=%llu cell_tput=%.3f Mbps jain=%.4f prb_util=%.4f\n",
               k.policy.c_str(), static_cast<unsigned long long>(k.slots),
               k.cell_throughput_bps / 1e6, k.jain_fairness, k.prb_utilization);
  return 0;
}

int run_live(const nrs::Config& cfg) {
  // Block termination signals in this thread *before* spawning workers so
  // every thread inherits the mask; then only sigwait() below receives them.
  // This keeps all shutdown logic in normal code, not in an async signal handler.
  sigset_t sigs;
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGTERM);
  sigaddset(&sigs, SIGINT);
  pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

  nrs::ConcurrentQueue<nrs::UeReport> queue(cfg.queue_capacity);
  nrs::Cell cell(cfg.cell);
  auto scheduler = nrs::make_scheduler(cfg.policy);
  nrs::MetricsHub hub;

  nrs::UdpReceiver receiver(cfg.udp_bind, cfg.udp_port, queue);
  nrs::SlotLoop slot_loop(cell, *scheduler, queue, hub);
  nrs::HttpServer http(cfg.http_bind, cfg.http_port, hub, [&receiver] { return receiver.stats(); });

  http.start();
  receiver.start();
  slot_loop.start();
  std::fprintf(stderr, "live: policy=%s prbs=%u udp=%s:%u http=%s:%u\n",
               std::string(nrs::to_string(cfg.policy)).c_str(), unsigned{cfg.cell.num_prbs},
               cfg.udp_bind.c_str(), unsigned{receiver.bound_port()}, cfg.http_bind.c_str(),
               unsigned{cfg.http_port});

  int sig = 0;
  sigwait(&sigs, &sig);
  std::fprintf(stderr, "received %s, shutting down\n", sig == SIGTERM ? "SIGTERM" : "SIGINT");

  // Stop the input first so no reports arrive after the final slot, and keep
  // HTTP up longest so metrics stay scrapeable while draining.
  hub.ready.store(false);
  receiver.stop();
  slot_loop.stop();
  http.stop();
  std::fprintf(stderr, "shutdown complete\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto args = parse_args(argc, argv);
    if (!args) {
      usage();
      return 2;
    }
    nrs::Config cfg = nrs::load_config(args->config_path, nrs::process_env());
    if (args->policy) {
      auto p = nrs::parse_policy(*args->policy);
      if (!p) throw std::invalid_argument("unknown policy " + *args->policy);
      cfg.policy = *p;
    }
    return args->mode == "offline" ? run_offline(cfg, *args) : run_live(cfg);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
  }
}
