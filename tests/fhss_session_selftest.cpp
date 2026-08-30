#include "FhssSession.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

int main() {
  devourer::HopSchedule::Key key{};
  for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(i + 1);

  devourer::FhssSession::Config follower_cfg;
  follower_cfg.role = devourer::FhssSession::Role::Follower;
  follower_cfg.channels = {36, 40, 44};
  follower_cfg.slot_ms = 20;
  follower_cfg.key = key;
  std::atomic<unsigned> follower_retunes{0};
  devourer::FhssSession follower(
      follower_cfg,
      [&](uint8_t, bool) { ++follower_retunes; return true; });

  devourer::FhssSession::Config authority_cfg = follower_cfg;
  authority_cfg.role = devourer::FhssSession::Role::Authority;
  std::atomic<unsigned> authority_retunes{0};
  devourer::FhssSession authority(
      authority_cfg,
      [&](uint8_t, bool) { ++authority_retunes; return true; },
      [&](const uint8_t* data, size_t size) {
        return follower.on_sync_marker(data, size);
      });

  follower.start();
  authority.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(240));
  const auto status = follower.status();
  authority.stop();
  follower.stop();

  if (status.state != devourer::FhssSession::State::Tracking) return EXIT_FAILURE;
  if (status.markers < 4 || follower_retunes < 4 || authority_retunes < 4)
    return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
