#include "CancellableThread.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>

int main() {
  std::atomic<int> stopped{0};
  auto worker = [&stopped](devourer::StopToken token) {
    while (!token.stop_requested())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++stopped;
  };
  {
    devourer::CancellableThread first(worker);
    devourer::CancellableThread second(worker);
    first = std::move(second); // cancels and joins the replaced worker
    assert(stopped == 1);
    devourer::CancellableThread moved(std::move(first));
  }
  assert(stopped == 2); // the moved worker is stopped and joined at scope exit
  std::cout << "Worker replacement, move and cooperative shutdown passed\n";
}
