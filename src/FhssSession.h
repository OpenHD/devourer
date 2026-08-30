#pragma once

#include "HopSchedule.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace devourer {

// Owns the lockstep hop clock and recovery scan.  Integrations provide only
// hardware operations; they must not run a second hop timer themselves.
class FhssSession {
 public:
  enum class Role { Authority, Follower };
  enum class State { Disabled, Authority, Acquiring, Tracking, Lost };

  struct Config {
    Role role = Role::Follower;
    std::vector<uint8_t> channels;
    uint32_t slot_ms = 50;
    HopSchedule::Key key{};
    bool cache_rf = true;
  };

  struct Status {
    State state = State::Disabled;
    uint64_t slot = 0;
    uint8_t channel = 0;
    uint64_t last_marker_age_ms = 0;
    int64_t phase_error_us = 0;
    uint64_t retunes = 0;
    uint64_t markers = 0;
    uint64_t reacquisitions = 0;
  };

  using RetuneCallback = std::function<bool(uint8_t, bool)>;
  using MarkerTxCallback = std::function<bool(const uint8_t*, size_t)>;

  FhssSession(Config config, RetuneCallback retune,
              MarkerTxCallback marker_tx = {});
  ~FhssSession();
  FhssSession(const FhssSession&) = delete;
  FhssSession& operator=(const FhssSession&) = delete;

  bool start();
  void stop();

  // Feed a marker vendor IE (or a complete frame containing one).  Returns
  // true only when a valid marker for this session was consumed.
  bool on_sync_marker(const uint8_t* data, size_t size);
  Status status() const;
  bool running() const { return running_.load(); }

 private:
  static int64_t now_us();
  void authority_loop();
  void follower_loop();
  bool retune(uint64_t slot, uint8_t channel);
  bool wait_until_us(int64_t target_us);

  const Config config_;
  const HopSchedule schedule_;
  RetuneCallback retune_cb_;
  MarkerTxCallback marker_tx_cb_;
  std::atomic<bool> running_{false};
  std::thread worker_;

  mutable std::mutex mutex_;
  Status status_;
  int64_t anchor_us_ = 0;
  int64_t last_marker_us_ = 0;
  uint32_t epoch_ = 0;
};

}  // namespace devourer
