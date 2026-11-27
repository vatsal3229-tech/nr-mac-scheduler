#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "runtime/concurrent_queue.h"

namespace nrs {

TEST(ConcurrentQueue, BoundedTryPush) {
  ConcurrentQueue<int> q(2);
  EXPECT_TRUE(q.try_push(1));
  EXPECT_TRUE(q.try_push(2));
  EXPECT_FALSE(q.try_push(3));
  std::vector<int> out;
  EXPECT_EQ(q.drain(out), 2u);
  EXPECT_EQ(out, (std::vector<int>{1, 2}));
  EXPECT_EQ(q.size(), 0u);
  EXPECT_TRUE(q.try_push(4));
}

// Several producers and one draining consumer, like the receiver and slot
// loop. Run under the tsan preset to prove there is no data race.
TEST(ConcurrentQueue, ConcurrentProducersLoseNothingAccepted) {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 20000;
  ConcurrentQueue<int> q(1000);
  std::atomic<int> accepted{0};
  std::atomic<bool> done{false};
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&] {
      for (int i = 0; i < kPerProducer; ++i) {
        if (q.try_push(i)) accepted.fetch_add(1);
      }
    });
  }
  size_t consumed = 0;
  std::thread consumer([&] {
    std::vector<int> buf;
    while (!done.load()) {
      buf.clear();
      consumed += q.drain(buf);
      std::this_thread::yield();
    }
    buf.clear();
    consumed += q.drain(buf);
  });
  for (auto& t : producers) t.join();
  done.store(true);
  consumer.join();
  EXPECT_EQ(consumed, static_cast<size_t>(accepted.load()));
  EXPECT_GT(accepted.load(), 0);
}

}  // namespace nrs
