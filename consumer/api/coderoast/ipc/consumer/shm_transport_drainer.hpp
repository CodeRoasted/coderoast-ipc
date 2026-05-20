#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/frame.hpp"

namespace coderoast::ipc::consumer
{

/// **Step 1 of the pull-based causal SHM consumer pipeline.**
///
/// Owns one background `std::jthread` per shard. Each thread spins on
/// `SharedMemorySpscChannel::try_pop`, moves any received non-EOS frame
/// into an in-process per-shard FIFO, and terminates on EOS after marking
/// the shard as done.
///
/// **Strict invariants (do NOT relax):**
///   * The drain loop never depends on causal ordering, watermarks, or
///     reorder-buffer state. It frees SHM slots as fast as the producer
///     fills them so the producer never blocks on backpressure.
///   * EOS frames are NOT forwarded into the per-shard FIFO. They are
///     reported via the `shard_eos` flag only. Downstream stages observe
///     end-of-stream by polling `transport_complete()` together with the
///     reorder buffer being empty.
///   * Window-seal frames ARE forwarded (they carry watermark information
///     consumers may want) but they do not set `shard_ever_had_data`.
///
/// **Thread-safety:** Per-shard FIFO is guarded by a mutex. A single
/// consumer thread (the reorder buffer) calls `try_pull` for a given shard;
/// the corresponding drain thread is the only writer. Other shards are
/// independent.
///
/// **Lifetime:** Drain threads start on construction. `close()` (also
/// invoked by the destructor) stops and joins them, then closes channels.
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
        threads_.reserve(config_.shard_count);
        for (std::size_t shard_id{0}; shard_id < config_.shard_count; ++shard_id)
        {
            threads_.emplace_back([this, shard_id](std::stop_token st)
                                  { drain_loop(shard_id, st); });
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

    /// Pop the next frame from shard `shard_id`'s in-process FIFO. Returns
    /// false immediately if the FIFO is empty (never blocks).
    [[nodiscard]] bool try_pull(std::size_t shard_id, Frame& out)
    {
        auto& shard{*shards_[shard_id]};
        std::lock_guard<std::mutex> lock{shard.mutex};
        if (shard.queue.empty())
        {
            return false;
        }
        out = std::move(shard.queue.front());
        shard.queue.pop_front();
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

    [[nodiscard]] std::size_t shard_buffered(std::size_t shard_id) const
    {
        auto& shard{*shards_[shard_id]};
        std::lock_guard<std::mutex> lock{shard.mutex};
        return shard.queue.size();
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

    /// Stop drain threads, join, then close channels. Idempotent.
    void close() noexcept
    {
        for (auto& thread : threads_)
        {
            thread.request_stop();
        }
        for (auto& thread : threads_)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
        threads_.clear();
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
        mutable std::mutex mutex;
        std::deque<Frame> queue;
        std::atomic<bool> eos{false};
        std::atomic<bool> ever_had_data{false};
    };

    void drain_loop(std::size_t shard_id, std::stop_token stop_token)
    {
        Frame frame{};
        auto& chan{channels_[shard_id]};
        auto& shard{*shards_[shard_id]};
        while (!stop_token.stop_requested())
        {
            if (chan.try_pop(frame))
            {
                const bool is_eos{coderoast::ipc::has_flag(
                    frame.header.flags, coderoast::ipc::LineFrameFlags::kLineFrameFlagEndOfStream)};
                if (is_eos)
                {
                    shard.eos.store(true, std::memory_order_release);
                    return;
                }
                const bool is_seal{coderoast::ipc::has_flag(
                    frame.header.flags, coderoast::ipc::LineFrameFlags::kLineFrameFlagWindowSeal)};
                {
                    std::lock_guard<std::mutex> lock{shard.mutex};
                    shard.queue.push_back(std::move(frame));
                }
                if (!is_seal)
                {
                    shard.ever_had_data.store(true, std::memory_order_release);
                }
            }
            else
            {
                std::this_thread::yield();
            }
        }
    }

    Config config_{};
    std::vector<Channel> channels_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::vector<std::jthread> threads_;
};

} // namespace coderoast::ipc::consumer
