#include "runtime/udp_receiver.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>

#include <cerrno>
#include <system_error>
#include <vector>

namespace nrs {
namespace {

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

UdpReceiver::UdpReceiver(const std::string& bind_addr, uint16_t port,
                         ConcurrentQueue<UeReport>& queue)
    : queue_(queue) {
  sock_ = UniqueFd(::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!sock_.valid()) throw_errno("socket");
  // A bigger kernel buffer absorbs bursts while the receiver thread is descheduled.
  int rcvbuf = 4 * 1024 * 1024;
  ::setsockopt(sock_.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
    throw std::system_error(EINVAL, std::generic_category(), "bad udp_bind " + bind_addr);
  }
  if (::bind(sock_.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    throw_errno("bind udp " + bind_addr + ":" + std::to_string(port));
  }
  socklen_t len = sizeof(addr);
  ::getsockname(sock_.get(), reinterpret_cast<sockaddr*>(&addr), &len);
  bound_port_ = ntohs(addr.sin_port);

  epoll_ = UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_.valid()) throw_errno("epoll_create1");
  wake_ = UniqueFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!wake_.valid()) throw_errno("eventfd");

  for (int fd : {sock_.get(), wake_.get()}) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) throw_errno("epoll_ctl");
  }
}

UdpReceiver::~UdpReceiver() { stop(); }

void UdpReceiver::start() { thread_ = std::thread([this] { run(); }); }

void UdpReceiver::stop() {
  if (!thread_.joinable()) return;
  const uint64_t one = 1;
  // Ignoring the result is safe: the only failure is a full counter, which still wakes epoll.
  [[maybe_unused]] ssize_t n = ::write(wake_.get(), &one, sizeof(one));
  thread_.join();
}

void UdpReceiver::run() {
  std::array<epoll_event, 4> events{};
  for (;;) {
    const int n = ::epoll_wait(epoll_.get(), events.data(), static_cast<int>(events.size()), -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;  // epoll fd broken: nothing sensible left to do on this thread
    }
    for (int i = 0; i < n; ++i) {
      if (events[static_cast<size_t>(i)].data.fd == wake_.get()) return;
      drain_socket();
    }
  }
}

void UdpReceiver::drain_socket() {
  // Max UDP payload; edge cases (oversized datagrams) are caught by the length check.
  static thread_local std::vector<uint8_t> buf(65536);
  std::vector<UeReport> parsed;
  for (;;) {
    const ssize_t len = ::recv(sock_.get(), buf.data(), buf.size(), 0);
    if (len < 0) return;  // EAGAIN: socket drained (level-triggered epoll re-arms)
    datagrams_.fetch_add(1, std::memory_order_relaxed);
    parsed.clear();
    const wire::ParseError err =
        wire::parse_datagram(buf.data(), static_cast<size_t>(len), parsed);
    if (err != wire::ParseError::kOk) {
      malformed_[static_cast<size_t>(err)].fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    for (const UeReport& r : parsed) {
      if (queue_.try_push(r)) {
        reports_.fetch_add(1, std::memory_order_relaxed);
      } else {
        queue_drops_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
}

ReceiverStats UdpReceiver::stats() const {
  // Relaxed loads: these are monotonic counters for monitoring, not synchronisation.
  ReceiverStats s;
  s.datagrams = datagrams_.load(std::memory_order_relaxed);
  s.reports = reports_.load(std::memory_order_relaxed);
  s.queue_drops = queue_drops_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < s.malformed.size(); ++i) {
    s.malformed[i] = malformed_[i].load(std::memory_order_relaxed);
  }
  return s;
}

}  // namespace nrs
