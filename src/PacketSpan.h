#pragma once

#include <cassert>
#include <cstddef>
#if __has_include(<span>) && !defined(DEVOURER_FORCE_PACKET_SPAN_FALLBACK)
#include <span>
#endif

namespace devourer {
#if defined(__cpp_lib_span) && !defined(DEVOURER_FORCE_PACKET_SPAN_FALLBACK)
template <typename T> using PacketSpan = std::span<T>;
#else
// The GCC 8 vendor SDK has no std::span. This is the non-owning, dynamic
// packet view used by the RX parser, not a replacement for the complete API.
// Its buffer must outlive the view; slicing has the same bounds preconditions.
template <typename T> class PacketSpan {
 public:
  constexpr PacketSpan() noexcept = default;
  constexpr PacketSpan(T* data, std::size_t size) noexcept
      : data_(data), size_(size) {}
  constexpr T* data() const noexcept { return data_; }
  constexpr std::size_t size() const noexcept { return size_; }
  constexpr bool empty() const noexcept { return size_ == 0; }
  constexpr T& operator[](std::size_t index) const noexcept {
    assert(index < size_);
    return data_[index];
  }
  constexpr PacketSpan subspan(std::size_t offset,
                              std::size_t count) const noexcept {
    assert(offset <= size_ && count <= size_ - offset);
    return {offset == 0 ? data_ : data_ + offset, count};
  }
 private:
  T* data_ = nullptr;
  std::size_t size_ = 0;
};
#endif
}  // namespace devourer
