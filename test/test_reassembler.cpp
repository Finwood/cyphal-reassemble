#include <gtest/gtest.h>

#include <vector>

#include "reassembler.h"

namespace {
// Build a single-frame CAN payload: data bytes followed by a tail byte
// with SOT|EOT|TOGGLE set and the given transfer_id.
std::vector<uint8_t> single_frame(std::vector<uint8_t> data, uint8_t transfer_id) {
    uint8_t tail = 0x80U | 0x40U | 0x20U | (transfer_id & 0x1FU);
    data.push_back(tail);
    return data;
}

std::vector<Transfer> collect(Reassembler& r, int64_t ts, uint32_t can_id,
                              const std::vector<uint8_t>& payload) {
    std::vector<Transfer> out;
    r.ingest(ts, can_id, payload.data(), payload.size(), 0U,
             [&](const Transfer& t) { out.push_back(t); });
    return out;
}
}  // namespace

TEST(Reassembler, SingleFrameHeartbeatMessage) {
    Reassembler r;
    auto frame = single_frame({0x55, 0x02, 0x00, 0x00, 0x00, 0x00, 0x09}, 21);
    auto out = collect(r, 1000, 0x101D552AU, frame);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].type, "Message");
    EXPECT_EQ(out[0].id, 7509);
    ASSERT_TRUE(out[0].source.has_value());
    EXPECT_EQ(out[0].source.value(), 42);
    EXPECT_FALSE(out[0].dest.has_value());
    EXPECT_EQ(out[0].priority, 4);
    EXPECT_EQ(out[0].transfer_id, 21);
    EXPECT_EQ(out[0].length, 7);
    EXPECT_EQ(out[0].payload, (std::vector<uint8_t>{0x55, 0x02, 0x00, 0x00, 0x00, 0x00, 0x09}));
    EXPECT_EQ(r.stats().frames_in, 1U);
    EXPECT_EQ(r.stats().transfers_out, 1U);
}

TEST(Reassembler, SingleFrameServiceResponseIsCapturedPromiscuously) {
    Reassembler r;
    auto frame = single_frame({0xAA, 0xBB}, 3);
    auto out = collect(r, 2000, 0x126B957DU, frame);  // service 430, 125 -> 42
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].type, "Response");
    EXPECT_EQ(out[0].id, 430);
    ASSERT_TRUE(out[0].source.has_value());
    EXPECT_EQ(out[0].source.value(), 125);
    ASSERT_TRUE(out[0].dest.has_value());
    EXPECT_EQ(out[0].dest.value(), 42);
    EXPECT_EQ(out[0].transfer_id, 3);
    EXPECT_EQ(out[0].payload, (std::vector<uint8_t>{0xAA, 0xBB}));
}

TEST(Reassembler, SingleFrameServiceRequestToDifferentDest) {
    Reassembler r;
    auto frame = single_frame({0x01}, 7);
    auto out = collect(r, 3000, 0x136BBEAAU, frame);  // service 430, 42 -> 125
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].type, "Request");
    EXPECT_EQ(out[0].id, 430);
    ASSERT_TRUE(out[0].source.has_value());
    EXPECT_EQ(out[0].source.value(), 42);
    ASSERT_TRUE(out[0].dest.has_value());
    EXPECT_EQ(out[0].dest.value(), 125);
    EXPECT_EQ(out[0].transfer_id, 7);
}

TEST(Reassembler, InvalidFrameLayoutIsCountedAsError) {
    Reassembler r;
    // Service frame with source == dest is invalid.
    uint32_t id = (4U << 26) | (1U << 25) | (430U << 14) | (42U << 7) | 42U;
    auto frame = single_frame({0x01}, 1);
    auto out = collect(r, 4000, id, frame);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(r.stats().errors, 1U);
    EXPECT_EQ(r.stats().transfers_out, 0U);
}
