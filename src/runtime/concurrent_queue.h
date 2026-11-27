#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace nrs {

// Bounded multi-producer queue drained in bulk by one consumer.
//
// Why a mutex and not a lock-free ring: the slot loop takes the lock once per
// 1 ms slot and the receiver once per datagram, so contention is negligible
// and a mutex is easy to verify with TSan. Why bounded with try_push: the
// receiver must never block (it would stop draining the socket), and memory
// must not grow without limit if the slot loop stalls; excess reports are
// dropped and counted instead.
template <typename T>
class ConcurrentQueue {
 public:
  explicit ConcurrentQueue(size_t capacity) : capacity_(capacity) {}

  bool try_push(const T& v) {
    std::lock_guard<std::mutex> lock(mu_);
    if (items_.size() >= capacity_) return false;
    items_.push_back(v);
    return true;
  }

  // Moves every queued item into `out` (appending). Returns how many were moved.
  size_t drain(std::vector<T>& out) {
    std::lock_guard<std::mutex> lock(mu_);
    const size_t n = items_.size();
    out.insert(out.end(), items_.begin(), items_.end());
    items_.clear();
    return n;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return items_.size();
  }

 private:
  const size_t capacity_;
  mutable std::mutex mu_;
  std::deque<T> items_;
};

}  // namespace nrs
