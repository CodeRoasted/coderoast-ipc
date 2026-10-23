// invariant: parse_shard_channel_name is the exact mirror of shard_channel_name -- it returns the
// pair the writer spelled a segment from, and refuses every name no pair spells.
// note: a unit test of two pure functions; no segment is created, no thread, no clock.
#include <gtest/gtest.h>

import coderoast.ipc.core.test;

namespace
{

using coderoast::ipc::parse_shard_channel_name;
using coderoast::ipc::shard_channel_name;
using coderoast::ipc::ShardChannelName;

[[nodiscard]] std::string render(const std::optional<ShardChannelName>& parsed)
{
    return parsed ? "{base \"" + std::string{parsed->base} + "\", shard " +
                        std::to_string(parsed->shard_id) + "}"
                  : std::string{"nullopt"};
}

struct WrittenName
{
    std::string_view base;
    std::size_t shard_id;
};

// invariant: an empty base, a base that itself carries the marker, and the widest shard number.
constexpr std::array<WrittenName, 5> kWrittenNames{{
    {.base = "insight_main", .shard_id = 0U},
    {.base = "insight_main", .shard_id = 17U},
    {.base = "", .shard_id = 3U},
    {.base = "outer_shard_1", .shard_id = 2U},
    {.base = "wide", .shard_id = std::numeric_limits<std::size_t>::max()},
}};

// invariant: one spelling per refusal class the parse's contract names.
constexpr std::array<std::string_view, 8> kUnwrittenNames{{
    "insight_main",
    "insight_main_shard_",
    "insight_main_shard_03",
    "insight_main_shard_+3",
    "insight_main_shard_-3",
    "insight_main_shard_3.lock",
    "insight_main_shard_3_",
    "insight_main_shard_99999999999999999999999",
}};

} // namespace

TEST(ShardChannelName, ParseReturnsThePairEveryWrittenNameWasSpelledFrom)
{
    for (const WrittenName& written : kWrittenNames)
    {
        const std::string segment{shard_channel_name(written.base, written.shard_id)};
        const auto parsed{parse_shard_channel_name(segment)};
        const ShardChannelName expected{.base = written.base, .shard_id = written.shard_id};
        EXPECT_EQ(parsed, std::optional{expected})
            << "segment \"" << segment << "\" parsed to " << render(parsed) << ", expected "
            << render(expected);
    }
}

TEST(ShardChannelName, ParseRefusesEveryNameNoPairSpells)
{
    for (const std::string_view segment : kUnwrittenNames)
    {
        const auto parsed{parse_shard_channel_name(segment)};
        EXPECT_FALSE(parsed.has_value())
            << "segment \"" << segment << "\" parsed to " << render(parsed)
            << ", expected nullopt: shard_channel_name never writes this spelling";
    }
}
