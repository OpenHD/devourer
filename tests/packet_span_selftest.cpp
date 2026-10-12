#include "PacketSpan.h"
#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
  devourer::PacketSpan<uint8_t> empty;
  assert(empty.empty() && empty.data() == nullptr);
  assert(empty.subspan(0, 0).empty());
  uint8_t bytes[] = {1, 2, 3, 4};
  devourer::PacketSpan<uint8_t> packet(bytes, sizeof(bytes));
  auto payload = packet.subspan(1, 2);
  assert(payload.data() == bytes + 1 && payload.size() == 2);
  payload[1] = 7;
  assert(bytes[2] == 7 && packet[2] == 7);
  auto tail = packet.subspan(4, 0);
  assert(tail.empty() && tail.data() == bytes + 4);
  auto copy = packet;
  assert(copy.data() == packet.data() && copy.size() == packet.size());
  std::cout << "Packet view lifetime, slicing and mutable access passed\n";
}
