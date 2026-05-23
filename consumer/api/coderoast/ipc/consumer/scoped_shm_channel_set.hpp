#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/frame.hpp"

namespace coderoast::ipc::consumer
{

/// RAII guard that unlinks every shard of a named sharded SHM channel set
/// on construction and again on destruction.
///
/// Use this around any consumer that owns the lifetime of its channel set
/// (tests, standalone consumer programs, scenario runners). The pre-unlink
/// ensures stale frames from a previously crashed run cannot leak into the
/// current run; the post-unlink ensures the next run starts clean.
///
/// This type intentionally lives in coderoast-ipc rather than in any
/// downstream test helper so that the convention is owned and tested by
/// the IPC package itself.
template <typename Frame = coderoast::ipc::DefaultLineFrame> class ScopedShmChannelSet
{
  public:
    using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;

    ScopedShmChannelSet(std::string channel_name, std::size_t shard_count)
        : channel_name_{std::move(channel_name)}, shard_count_{shard_count}
    {
        unlink_all();
    }

    ScopedShmChannelSet(const ScopedShmChannelSet&) = delete;
    ScopedShmChannelSet& operator=(const ScopedShmChannelSet&) = delete;
    ScopedShmChannelSet(ScopedShmChannelSet&&) = delete;
    ScopedShmChannelSet& operator=(ScopedShmChannelSet&&) = delete;

    ~ScopedShmChannelSet() noexcept
    {
        unlink_all();
    }

    [[nodiscard]] const std::string& channel_name() const noexcept
    {
        return channel_name_;
    }

    [[nodiscard]] std::size_t shard_count() const noexcept
    {
        return shard_count_;
    }

    [[nodiscard]] static std::string shard_channel_name(std::string_view base, std::size_t shard_id)
    {
        return std::string{base} + "_shard_" + std::to_string(shard_id);
    }

  private:
    void unlink_all() const noexcept
    {
        for (std::size_t shard_id{0}; shard_id < shard_count_; ++shard_id)
        {
            Channel::unlink(shard_channel_name(channel_name_, shard_id));
        }
    }

    std::string channel_name_;
    std::size_t shard_count_{0};
};

} // namespace coderoast::ipc::consumer
