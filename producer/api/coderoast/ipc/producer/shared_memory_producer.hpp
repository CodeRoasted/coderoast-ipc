#pragma once

// coderoast/ipc/producer/shared_memory_producer.hpp
//
// SharedMemoryProducer — helper for building and sequencing frames into
// shared-memory channels. Header-only implementation suitable for embedding
// in any producer sink or adapter.
//
// Responsibilities:
//   * Global + per-shard sequence tracking
//   * Frame header construction
//   * Format/policy enum mapping
//   * Agent ID stable hashing

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/frame.hpp"

namespace coderoast::ipc::producer {

// Stable per-agent hash for frame headers (portable across restarts).
// Uses FNV-1a 32-bit.
[[nodiscard]] inline std::uint32_t
stable_agent_id(std::string_view agent_name) noexcept {
  constexpr std::uint32_t kOffsetBasis{2166136261U};
  constexpr std::uint32_t kPrime{16777619U};
  std::uint32_t hash{kOffsetBasis};
  for (const unsigned char byte : agent_name) {
    hash ^= byte;
    hash *= kPrime;
  }
  return hash;
}

// Frame builder: tracks global + per-shard sequences and assembles frame
// headers. Callers provide the payload bytes; this class handles sequencing and
// header setup.
template <typename Frame = DefaultLineFrame> class FrameBuilder {
public:
  struct Config {
    std::size_t shard_count{1};
    std::uint64_t first_sequence{1};
  };

  explicit FrameBuilder(Config cfg) : shard_count_{cfg.shard_count} {
    global_sequence_.store(cfg.first_sequence - 1U, std::memory_order_relaxed);
    shard_sequences_.assign(cfg.shard_count, 0U);
  }

  FrameBuilder(const FrameBuilder &) = delete;
  FrameBuilder &operator=(const FrameBuilder &) = delete;
  FrameBuilder(FrameBuilder &&) = default;
  FrameBuilder &operator=(FrameBuilder &&) = default;
  ~FrameBuilder() = default;

  // Build a frame header with the next global and per-shard sequences.
  // Caller provides shard_id (modulo'd vs shard_count internally),
  // timestamp, payload_size, agent ID, format, and frame flags.
  // Returns the assembled frame with populated header.
  [[nodiscard]] Frame build(std::uint32_t shard_id,
                            std::uint64_t timestamp_unix_ns,
                            std::uint32_t payload_size,
                            std::uint32_t agent_id_hash, FrameFormat format,
                            LineFrameFlags flags = LineFrameFlags{}) noexcept {
    const auto normalized_shard{shard_id % shard_count_};

    Frame frame{};
    frame.header.sequence =
        global_sequence_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    frame.header.shard_sequence = ++shard_sequences_[normalized_shard];
    frame.header.timestamp_unix_ns = timestamp_unix_ns;
    frame.header.payload_size = payload_size;
    frame.header.agent_id = agent_id_hash;
    frame.header.shard_id = static_cast<std::uint32_t>(normalized_shard);
    frame.header.format = format;
    frame.header.flags = flags;
    return frame;
  }

  [[nodiscard]] std::size_t shard_count() const noexcept {
    return shard_count_;
  }

  [[nodiscard]] std::uint64_t global_sequence() const noexcept {
    return global_sequence_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t
  shard_sequence(std::size_t shard_id) const noexcept {
    return shard_sequences_[shard_id % shard_count_];
  }

private:
  std::size_t shard_count_;
  std::atomic<std::uint64_t> global_sequence_{0};
  std::vector<std::uint64_t> shard_sequences_{};
};

} // namespace coderoast::ipc::producer
