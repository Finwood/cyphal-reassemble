#include <gtest/gtest.h>

#include "decode.h"
#include "mdf_builder.hpp"

TEST(Decode, ClassicRxFrame) {
    auto file = build_classic_rx(/*start_ns=*/1'700'000'000'000'000'000ull, /*raw_us=*/1000,
                                 /*bus=*/1, /*can_id=*/0x123, /*dlc=*/2, {0xaa, 0xbb});
    auto got = decode_mf4(file.data(), file.size());
    ASSERT_TRUE(got.error.empty()) << got.error;
    ASSERT_EQ(got.frames.size(), 1u);
    const auto& f = got.frames[0];
    EXPECT_EQ(f.channel, "CAN1");
    EXPECT_EQ(f.timestamp_us, 1'700'000'000'000'000ll + 1000);
    EXPECT_EQ(f.id, 0x123u);
    EXPECT_EQ(f.dlc, 2);
    EXPECT_FALSE(f.ide);
    EXPECT_FALSE(f.rtr);
    EXPECT_FALSE(f.err);
    EXPECT_FALSE(f.edl);
    EXPECT_FALSE(f.brs);
    EXPECT_FALSE(f.esi);
    EXPECT_FALSE(f.tx);
    EXPECT_EQ(f.data, std::vector<uint8_t>({0xaa, 0xbb}));
}
