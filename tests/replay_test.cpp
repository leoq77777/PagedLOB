#include "fastlob/lob_fast.hpp"
#include "fastlob/lob_reference.hpp"
#include "fastlob/replay.hpp"
#include "fastlob/synthetic_mbo.hpp"

#include <gtest/gtest.h>

#include <filesystem>

TEST(Replay, BinRoundTripAndDifferential) {
    fastlob::SyntheticConfig cfg;
    cfg.num_events = 5000;
    cfg.seed = 99;
    auto events = fastlob::generate_synthetic_mbo(cfg);

    const auto path = (std::filesystem::temp_directory_path() / "fastlob_mbo.bin").string();
    ASSERT_TRUE(fastlob::write_events_bin(path, events));

    std::vector<fastlob::BookEvent> loaded;
    ASSERT_TRUE(fastlob::read_events_bin(path, loaded));
    ASSERT_EQ(loaded.size(), events.size());

    fastlob::ReferenceOrderBook ref;
    fastlob::FastOrderBook fast(1 << 16, 1 << 12);
    fastlob::replay_into(ref, loaded);
    fastlob::replay_into(fast, loaded);
    EXPECT_EQ(ref.order_count(), fast.order_count());

    fastlob::LevelView a[8], b[8];
    EXPECT_EQ(ref.top_n(fastlob::Side::Buy, a), fast.top_n(fastlob::Side::Buy, b));
}
