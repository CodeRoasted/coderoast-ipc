#pragma once

// coderoast/ipc/consumer/shared_memory_source.hpp
//
// SharedMemorySource — consumer-side adapter over OrderedLineFrameIterator.
//
// Presents incoming IPC frames as a zero-copy string_view stream suitable
// for feeding to any raw-line consumer or sink.
//
// Usage:
//
//   SharedMemorySource<> source{SharedMemorySource<>::Config{
//       .channel     = "coderoast.myapp",
//       .shard_count = 4,
//   }};
//
//   std::string_view payload;
//   while (source.try_pop(payload)) {
//       pipeline.ingest_line(payload);   // payload valid until next try_pop()
//   }
//
// Ownership & validity:
//   The string_view returned via out_payload is a non-owning view into an
//   internal frame buffer.  It is valid only until the next call to
//   try_pop().  Callers that need to retain the data must copy it.
//
// Thread safety:
//   Not thread-safe.  Use one instance per thread or serialise externally.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/consumer/ordered_line_frame_iterator.hpp"
#include "coderoast/ipc/frame.hpp"

namespace coderoast::ipc::consumer
{

template <typename Frame = coderoast::ipc::DefaultLineFrame> class SharedMemorySource
{
  public:
    struct Config
    {
        std::string channel{"coderoast.default"};
        std::size_t shard_count{1};
        std::uint64_t first_sequence{1};
        SequenceGapPolicy gap_policy{SequenceGapPolicy::WaitForMissing};
        FrameOrdering ordering{FrameOrdering::CausalKey};
        coderoast::ipc::BackpressurePolicy backpressure{coderoast::ipc::BackpressurePolicy::Block};
        coderoast::ipc::WaitStrategy wait_strategy{coderoast::ipc::WaitStrategy::SpinYieldPark};
    };

    SharedMemorySource() = default;

    explicit SharedMemorySource(Config config)
        : iterator_{typename OrderedLineFrameIterator<Frame>::Config{
              .channel = std::move(config.channel),
              .shard_count = config.shard_count,
              .first_sequence = config.first_sequence,
              .gap_policy = config.gap_policy,
              .ordering = config.ordering,
              .backpressure = config.backpressure,
              .wait_strategy = config.wait_strategy,
          }}
    {
    }

    SharedMemorySource(const SharedMemorySource&) = delete;
    SharedMemorySource& operator=(const SharedMemorySource&) = delete;
    SharedMemorySource(SharedMemorySource&&) noexcept = default;
    SharedMemorySource& operator=(SharedMemorySource&&) noexcept = default;
    ~SharedMemorySource() = default;

    // Attempt to consume the next ordered frame.
    //
    // Returns true and populates out_payload with a view into the frame's
    // payload bytes.  The view is valid until the next call to try_pop().
    //
    // Returns false when no in-order frame is currently available (either
    // all channels are empty, or a gap is being waited on under the
    // WaitForMissing policy).
    [[nodiscard]] bool try_pop(std::string_view& out_payload)
    {
        while (iterator_.try_next(current_frame_))
        {
            if (coderoast::ipc::is_control_frame(current_frame_.header.flags))
            {
                continue;
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            out_payload =
                std::string_view{reinterpret_cast<const char*>(current_frame_.payload.data()),
                                 current_frame_.header.payload_size};
            return true;
        }
        return false;
    }

    // Attempt to consume the next ordered frame, including control frames such
    // as WindowSeal and EndOfStream. Callers that need deterministic replay
    // completion should use this API instead of the payload-only try_pop().
    [[nodiscard]] bool try_pop_frame(Frame& out_frame)
    {
        return iterator_.try_next(out_frame);
    }

    // Shard id carried in the frame header of the most recently popped frame.
    // Valid only immediately after a successful try_pop() call.
    [[nodiscard]] std::uint32_t current_shard_id() const noexcept
    {
        return current_frame_.header.shard_id;
    }

    // Number of globally-sequenced frames skipped due to SkipMissing policy.
    [[nodiscard]] std::uint64_t skipped_sequences() const noexcept
    {
        return iterator_.skipped_sequences();
    }

    // Number of shards this source is consuming.
    [[nodiscard]] std::size_t shard_count() const noexcept
    {
        return iterator_.shard_count();
    }

    void close() noexcept
    {
        iterator_.close();
    }

  private:
    OrderedLineFrameIterator<Frame> iterator_{};
    Frame current_frame_{};
};

} // namespace coderoast::ipc::consumer
