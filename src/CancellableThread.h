#pragma once
#include <atomic>
#include <memory>
#include <thread>
#include <utility>

namespace devourer {
#if defined(__cpp_lib_jthread) && !defined(DEVOURER_FORCE_THREAD_FALLBACK)
using CancellableThread = std::jthread;
using StopToken = std::stop_token;
#else
// Only the cooperative stop/join operation used by delayed ACK disarm workers.
class StopToken {
 public:
  explicit StopToken(std::shared_ptr<std::atomic<bool>> stop) : stop_(std::move(stop)) {}
  bool stop_requested() const noexcept { return stop_->load(); }
 private:
  std::shared_ptr<std::atomic<bool>> stop_;
};
class CancellableThread {
 public:
  CancellableThread() = default;
  template <typename Function>
  explicit CancellableThread(Function function)
      : stop_(std::make_shared<std::atomic<bool>>(false)),
        thread_([stop = stop_, function = std::move(function)]() mutable {
          function(StopToken(stop));
        }) {}
  CancellableThread(CancellableThread&&) noexcept = default;
  CancellableThread& operator=(CancellableThread&& other) noexcept {
    if (this != &other) {
      stop_and_join();
      stop_ = std::move(other.stop_);
      thread_ = std::move(other.thread_);
    }
    return *this;
  }
  ~CancellableThread() { stop_and_join(); }
 private:
  void stop_and_join() noexcept {
    if (thread_.joinable()) {
      stop_->store(true);
      thread_.join();
    }
  }
  std::shared_ptr<std::atomic<bool>> stop_;
  std::thread thread_;
};
#endif
}  // namespace devourer
