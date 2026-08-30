#include "FhssSession.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <stdexcept>
#include <utility>

namespace devourer {

FhssSession::FhssSession(Config config, RetuneCallback retune,
                         MarkerTxCallback marker_tx)
    : config_(std::move(config)),
      schedule_(config_.key),
      retune_cb_(std::move(retune)),
      marker_tx_cb_(std::move(marker_tx)) {
  if (config_.channels.empty())
    throw std::invalid_argument("FHSS needs at least one channel");
  if (config_.slot_ms < 10 || config_.slot_ms > 1000)
    throw std::invalid_argument("FHSS slot must be between 10 and 1000 ms");
  if (!retune_cb_)
    throw std::invalid_argument("FHSS needs a retune callback");
  if (config_.role == Role::Authority && !marker_tx_cb_)
    throw std::invalid_argument("FHSS authority needs a marker TX callback");
}

FhssSession::~FhssSession() { stop(); }

int64_t FhssSession::now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool FhssSession::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = {};
    anchor_us_ = 0;
    last_marker_us_ = 0;
    epoch_ = 0;
    status_.state = config_.role == Role::Authority ? State::Authority
                                                    : State::Acquiring;
  }
  try {
    worker_ = std::thread(config_.role == Role::Authority
                              ? &FhssSession::authority_loop
                              : &FhssSession::follower_loop,
                          this);
  } catch (...) {
    running_.store(false);
    throw;
  }
  return true;
}

void FhssSession::stop() {
  running_.store(false);
  if (worker_.joinable()) worker_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  status_.state = State::Disabled;
}

bool FhssSession::wait_until_us(const int64_t target_us) {
  while (running_.load()) {
    const auto remaining = target_us - now_us();
    if (remaining <= 0) return true;
    std::this_thread::sleep_for(
        std::chrono::microseconds(std::min<int64_t>(remaining, 1000)));
  }
  return false;
}

bool FhssSession::retune(const uint64_t slot, const uint8_t channel) {
  if (!retune_cb_(channel, config_.cache_rf)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  status_.slot = slot;
  status_.channel = channel;
  ++status_.retunes;
  return true;
}

void FhssSession::authority_loop() {
  const int64_t slot_us = static_cast<int64_t>(config_.slot_ms) * 1000;
  const int64_t origin = now_us();
  {
    std::random_device rd;
    std::lock_guard<std::mutex> lock(mutex_);
    epoch_ = (static_cast<uint32_t>(rd()) ^
              static_cast<uint32_t>(origin)) | 1u;
    anchor_us_ = origin;
  }
  uint64_t last_slot = UINT64_MAX;
  while (running_.load()) {
    const int64_t now = now_us();
    const uint64_t slot = static_cast<uint64_t>(std::max<int64_t>(0, now - origin) /
                                                slot_us);
    if (slot != last_slot) {
      const uint8_t channel = schedule_.channel(slot, config_.channels);
      retune(slot, channel);
      const uint32_t phase = static_cast<uint32_t>(
          std::clamp<int64_t>(now_us() - origin -
                                  static_cast<int64_t>(slot) * slot_us,
                              0, slot_us - 1));
      const auto marker = HopSyncMarker::encode(
          {schedule_.fingerprint(), epoch_, phase, slot});
      if (marker_tx_cb_(marker.data(), marker.size())) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.markers;
      }
      last_slot = slot;
    }
    // Re-announce at mid-slot so a follower scanning this channel has more
    // than a single boundary-sized opportunity to acquire the schedule.
    const int64_t boundary = origin + static_cast<int64_t>(slot) * slot_us;
    const int64_t midpoint = boundary + slot_us / 2;
    if (now < midpoint && wait_until_us(midpoint)) {
      const auto marker = HopSyncMarker::encode(
          {schedule_.fingerprint(), epoch_,
           static_cast<uint32_t>(std::max<int64_t>(0, now_us() - boundary)),
           slot});
      if (marker_tx_cb_(marker.data(), marker.size())) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.markers;
      }
    }
    wait_until_us(boundary + slot_us);
  }
}

bool FhssSession::on_sync_marker(const uint8_t* data, const size_t size) {
  if (!running_.load() || config_.role != Role::Follower || !data) return false;
  HopSyncMarker marker;
  if (!HopSyncMarker::decode(data, size, marker) ||
      marker.fingerprint != schedule_.fingerprint())
    return false;
  const int64_t slot_us = static_cast<int64_t>(config_.slot_ms) * 1000;
  if (marker.phase_us >= static_cast<uint64_t>(slot_us)) return false;
  const int64_t now = now_us();
  const int64_t observed = now - static_cast<int64_t>(marker.phase_us) -
                           static_cast<int64_t>(marker.slot) * slot_us;
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t error = 0;
  if (!anchor_us_ || epoch_ != marker.epoch) {
    anchor_us_ = observed;
  } else {
    error = std::clamp(observed - anchor_us_, int64_t{-2000}, int64_t{2000});
    anchor_us_ += error / 4;
  }
  epoch_ = marker.epoch;
  last_marker_us_ = now;
  status_.phase_error_us = error;
  ++status_.markers;
  return true;
}

void FhssSession::follower_loop() {
  const int64_t slot_us = static_cast<int64_t>(config_.slot_ms) * 1000;
  size_t scan = 0;
  uint64_t tuned_slot = UINT64_MAX;
  int64_t next_scan = 0;
  bool had_lock = false;
  while (running_.load()) {
    const int64_t now = now_us();
    int64_t anchor = 0;
    int64_t last_marker = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      anchor = anchor_us_;
      last_marker = last_marker_us_;
    }
    const bool tracking = anchor > 0 && last_marker > 0 &&
                          now - last_marker < 3 * slot_us;
    if (tracking) {
      if (!had_lock) had_lock = true;
      const uint64_t slot = static_cast<uint64_t>(std::max<int64_t>(0, now - anchor) /
                                                  slot_us);
      if (tuned_slot == UINT64_MAX || slot > tuned_slot) {
        retune(slot, schedule_.channel(slot, config_.channels));
        tuned_slot = slot;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = State::Tracking;
        status_.last_marker_age_ms = static_cast<uint64_t>(now - last_marker) / 1000;
      }
      wait_until_us(anchor + static_cast<int64_t>(slot + 1) * slot_us);
      continue;
    }

    if (had_lock) {
      std::lock_guard<std::mutex> lock(mutex_);
      status_.state = State::Lost;
      ++status_.reacquisitions;
      had_lock = false;
      tuned_slot = UINT64_MAX;
      next_scan = 0;
    }
    if (now >= next_scan) {
      const uint8_t channel = config_.channels[scan++ % config_.channels.size()];
      retune(0, channel);
      next_scan = now + 2 * slot_us;
      std::lock_guard<std::mutex> lock(mutex_);
      status_.state = State::Acquiring;
      status_.last_marker_age_ms = last_marker
                                       ? static_cast<uint64_t>(now - last_marker) / 1000
                                       : 0;
    }
    wait_until_us(std::min<int64_t>(next_scan, now + 1000));
  }
}

FhssSession::Status FhssSession::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = status_;
  if (last_marker_us_ > 0)
    result.last_marker_age_ms =
        static_cast<uint64_t>(std::max<int64_t>(0, now_us() - last_marker_us_)) /
        1000;
  return result;
}

}  // namespace devourer
