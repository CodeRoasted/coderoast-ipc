#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/frame.hpp"

namespace coderoast::ipc::consumer
{

/// **Step 1 of the pull-based causal SHM consumer pipeline.**
///
/// Owns one `SharedMemorySpscChannel` per shard. Holds **no background
/// threads, no mutex, no internal queue.** Each call to `try_pull` is a
/// direct, lock-free pop on the per-shard SPSC ring; the result is moved
/// straight into the caller-provided `Frame`. EOS frames are absorbed
/// (the per-shard `eos` flag is set) so callers never observe them.
///
/// **Why no threads?** The earlier design used N background threads each
/// pushing into a mutex-protected deque. That added:
///   * a cache-line ping-pong per frame between drain thread and consumer,
///   * a `std::deque` heap allocation per chunk of frames,
///   * a `std::this_thread::yield()` storm whenever shards were idle.
/// `SharedMemorySpscChannel::try_pop` is already lock-free and cheap;
/// round-robin polling N shards from a single consumer thread is bound
/// by memory bandwidth, not by `try_pop` cost. Collapsing to one thread
/// eliminates the FIFO copy step, removes mutex contention, and makes
/// the whole pipeline single-producer / single-consumer end-to-end.
///
/// **Strict invariants (do NOT relax):**
///   * `try_pull` never blocks. It returns `false` when the shard's SHM
///     ring is empty *or* the shard has already observed EOS.
///   * EOS frames are NOT returned to the caller. They flip the per-shard
///     `eos` flag and `try_pull` returns `false`.
///   * Window-seal frames ARE returned (they carry watermark information
///     downstream stages may want) but they do not set `ever_had_data`.
///   * No causal-ordering / watermark logic lives here — that is step 2's
///     responsibility. This stage only knows about SHM transport state.
///
/// **Thread-safety:** A single consumer thread per shard. Different
/// shards may in principle be polled by different consumer threads
/// because each shard owns its own SPSC channel and atomic flags, but
/// the recommended (and tested) usage drains all shards from one thread.
///
/// **Lifetime:** All channels are opened on construction and closed on
/// destruction. `close()` is idempotent and may be called manually.
template <typename Frame = coderoast::ipc::DefaultLineFrame> class ShmTransportDrainer
{
  public:
    using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;

    struct Config
    {
        std::string channel{"coderoast.default"};
        std::size_t shard_count{1};
        coderoast::ipc::BackpressurePolicy backpressure{coderoast::ipc::BackpressurePolicy::Block};
        coderoast::ipc::WaitStrategy wait_strategy{coderoast::ipc::WaitStrategy::SpinYieldPark};
    };

    explicit ShmTransportDrainer(Config config) : config_{std::move(config)}
    {
        if (config_.shard_count == 0U)
        {
            throw std::invalid_argument(
                "ShmTransportDrainer shard_count must be greater than zero");
        }
        channels_.reserve(config_.shard_count);
        shards_.reserve(config_.shard_count);
        for (std::size_t shard_id{0}; shard_id < config_.shard_count; ++shard_id)
        {
            channels_.emplace_back(Channel::open(shard_channel_name(config_.channel, shard_id),
                                                 config_.backpressure, config_.wait_strategy));
            shards_.emplace_back(std::make_unique<Shard>());
        }
    }

    ShmTransportDrainer(const ShmTransportDrainer&) = delete;
    ShmTransportDrainer& operator=(const ShmTransportDrainer&) = delete;
    ShmTransportDrainer(ShmTransportDrainer&&) = delete;
    ShmTransportDrainer& operator=(ShmTransportDrainer&&) = delete;

    ~ShmTransportDrainer()
    {
        close();
    }

    [[nodiscard]] std::size_t shard_count() const noexcept
    {
        return shards_.size();
    }

    /// Non-blocking pop from shard `shard_id`'s SPSC SHM ring. Returns
    /// `false` immediately if the ring is empty, the shard has already
    /// observed EOS, or the popped frame WAS the EOS marker (in which
    /// case the shard's `eos` flag is set as a side-effect).
    [[nodiscard]] bool try_pull(std::size_t shard_id, Frame& out)
    {
        auto& shard{*shards_[shard_id]};
        if (shard.eos.load(std::memory_order_acquire))
        {
            return false;
        }
        if (!channels_[shard_id].try_pop(out))
        {
            return false;
        }
        const bool is_eos{coderoast::ipc::has_flag(
            out.header.flags, coderoast::ipc::LineFrameFlags::kLineFrameFlagEndOfStream)};
        if (is_eos)
        {
            shard.eos.store(true, std::memory_order_release);
            return false;
        }
        const bool is_seal{coderoast::ipc::has_flag(
            out.header.flags, coderoast::ipc::LineFrameFlags::kLineFrameFlagWindowSeal)};
        if (!is_seal)
        {
            shard.ever_had_data.store(true, std::memory_order_release);
        }
        return true;
    }

    [[nodiscard]] bool shard_eos(std::size_t shard_id) const noexcept
    {
        return shards_[shard_id]->eos.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool shard_ever_had_data(std::size_t shard_id) const noexcept
    {
        return shards_[shard_id]->ever_had_data.load(std::memory_order_acquire);
    }

    /// True once every shard has observed EOS. The reorder buffer must
    /// still drain its own per-shard min-heaps after this returns true.
    [[nodiscard]] bool transport_complete() const noexcept
    {
        for (const auto& shard : shards_)
        {
            if (!shard->eos.load(std::memory_order_acquire))
            {
                return false;
            }
        }
        return !shards_.empty();
    }

    [[nodiscard]] std::vector<coderoast::ipc::ChannelStats> channel_stats() const
    {
        std::vector<coderoast::ipc::ChannelStats> out;
        out.reserve(channels_.size());
        for (const auto& chan : channels_)
        {
            out.push_back(chan.stats());
        }
        return out;
    }

    /// Close every channel. Idempotent.
    void close() noexcept
    {
        for (auto& chan : channels_)
        {
            chan.close();
        }
        channels_.clear();
    }

    [[nodiscard]] static std::string shard_channel_name(std::string_view base, std::size_t shard_id)
    {
        return std::string{base} + "_shard_" + std::to_string(shard_id);
    }

  private:
    struct Shard
    {
        std::atomic<bool> eos{false};
        std::atomic<bool> ever_had_data{false};
    };

    Config config_{};
    std::vector<Channel> channels_;
    std::vector<std::unique_ptr<Shard>> shards_;
};

} // namespace coderoast::ipc::consumer
