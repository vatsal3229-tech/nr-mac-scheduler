#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "core/types.h"
#include "proto/wire.h"
#include "runtime/concurrent_queue.h"
#include "runtime/receiver_stats.h"
#include "runtime/unique_fd.h"

namespace nrs {

// Receives UE report datagrams on its own thread and pushes parsed reports
// into the queue. Uses epoll on the socket plus an eventfd so stop() can wake
// the thread immediately instead of waiting for a poll timeout.
class UdpReceiver {
 public:
  // Binds immediately (throws std::system_error on failure) so a bad port
  // fails at startup, before readiness is reported.
  UdpReceiver(const std::string& bind_addr, uint16_t port, ConcurrentQueue<UeReport>& queue);
  ~UdpReceiver();
  UdpReceiver(const UdpReceiver&) = delete;
  UdpReceiver& operator=(const UdpReceiver&) = delete;

  void start();
  void stop();
  uint16_t bound_port() const { return bound_port_; }
  ReceiverStats stats() const;

 private:
  void run();
  void drain_socket();

  ConcurrentQueue<UeReport>& queue_;
  UniqueFd sock_;
  UniqueFd epoll_;
  UniqueFd wake_;
  uint16_t bound_port_ = 0;
  std::thread thread_;

  std::atomic<uint64_t> datagrams_{0};
  std::atomic<uint64_t> reports_{0};
  std::atomic<uint64_t> queue_drops_{0};
  std::array<std::atomic<uint64_t>, wire::kParseErrorCount> malformed_{};
};

}  // namespace nrs
