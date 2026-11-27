#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "runtime/metrics.h"
#include "runtime/receiver_stats.h"

namespace httplib {
class Server;
}

namespace nrs {

// /metrics, /healthz (liveness) and /readyz (readiness) on one port.
//   liveness  = the slot loop is still making progress (a hung loop gets the pod restarted)
//   readiness = sockets bound and at least one slot published (no traffic before that)
class HttpServer {
 public:
  HttpServer(std::string bind, uint16_t port, MetricsHub& hub,
             std::function<ReceiverStats()> rx_stats);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  void start();  // throws std::runtime_error if the port cannot be bound
  void stop();

 private:
  std::unique_ptr<httplib::Server> server_;
  std::string bind_;
  uint16_t port_;
  MetricsHub& hub_;
  std::function<ReceiverStats()> rx_stats_;
  std::thread thread_;
};

}  // namespace nrs
