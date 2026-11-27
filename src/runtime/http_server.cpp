#include "runtime/http_server.h"

#include <httplib.h>

#include <chrono>
#include <stdexcept>

#include "runtime/prometheus.h"

namespace nrs {
namespace {
// A slot loop that has not completed a slot for this long is considered hung.
constexpr auto kLivenessTimeout = std::chrono::seconds(2);
}  // namespace

HttpServer::HttpServer(std::string bind, uint16_t port, MetricsHub& hub,
                       std::function<ReceiverStats()> rx_stats)
    : server_(std::make_unique<httplib::Server>()),
      bind_(std::move(bind)),
      port_(port),
      hub_(hub),
      rx_stats_(std::move(rx_stats)) {
  server_->Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
    res.set_content(render_prometheus(hub_.snapshot(), rx_stats_()),
                    "text/plain; version=0.0.4; charset=utf-8");
  });
  server_->Get("/healthz", [this](const httplib::Request&, httplib::Response& res) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto last = std::chrono::nanoseconds(hub_.last_slot_ns.load());
    const bool alive = last.count() != 0 && (now - last) < kLivenessTimeout;
    res.status = alive ? 200 : 503;
    res.set_content(alive ? "ok\n" : "slot loop stalled\n", "text/plain");
  });
  server_->Get("/readyz", [this](const httplib::Request&, httplib::Response& res) {
    const bool ready = hub_.ready.load();
    res.status = ready ? 200 : 503;
    res.set_content(ready ? "ready\n" : "not ready\n", "text/plain");
  });
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
  if (!server_->bind_to_port(bind_, port_)) {
    throw std::runtime_error("cannot bind http " + bind_ + ":" + std::to_string(port_));
  }
  thread_ = std::thread([this] { server_->listen_after_bind(); });
}

void HttpServer::stop() {
  if (!thread_.joinable()) return;
  server_->stop();
  thread_.join();
}

}  // namespace nrs
