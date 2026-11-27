#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

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

TEST(Decode, ErrorTypeBecomesSocketcan) {
    // ErrorType 5, bus 2, raw_us 50, start_ns 0
    // id 0x20, data 00 00 01 19 00 00 00 00, dlc 8, err true, other flags false
    // channel CAN2, timestamp_us 50
    MdfBuilder builder;
    builder.start_ns = 0;
    builder.add_error_group(2, 2);
    builder.add_error_record(2, 50, 5);
    auto file = builder.build();
    auto got = decode_mf4(file.data(), file.size());
    ASSERT_TRUE(got.error.empty()) << got.error;
    ASSERT_EQ(got.frames.size(), 1u);
    const auto& f = got.frames[0];
    EXPECT_EQ(f.channel, "CAN2");
    EXPECT_EQ(f.timestamp_us, 50);
    EXPECT_EQ(f.id, 0x20u);
    EXPECT_EQ(f.dlc, 8);
    EXPECT_FALSE(f.ide);
    EXPECT_FALSE(f.rtr);
    EXPECT_TRUE(f.err);
    EXPECT_FALSE(f.edl);
    EXPECT_FALSE(f.brs);
    EXPECT_FALSE(f.esi);
    EXPECT_FALSE(f.tx);
    EXPECT_EQ(f.data, (std::vector<uint8_t>{0x00, 0x00, 0x01, 0x19, 0x00, 0x00, 0x00, 0x00}));
}

TEST(Decode, VlsdPayloadIgnoresFourBitDataLength) {
    // DLC nibble 15, VLSD entry length 12, 12 payload bytes 1..12
    // offset stored in the frame is 0
    // data size 12, edl true, dlc 15
    std::vector<uint8_t> payload(12);
    for (uint8_t i = 0; i < 12; ++i) payload[i] = static_cast<uint8_t>(i + 1);
    MdfBuilder builder;
    builder.add_fd_group(3, 4, 1);
    builder.add_fd_record(3, 0, 0x42, /*dlc=*/15, /*vlsd_offset=*/0, false, false);
    builder.add_vlsd_record(4, payload);
    auto file = builder.build();
    auto got = decode_mf4(file.data(), file.size());
    ASSERT_TRUE(got.error.empty()) << got.error;
    ASSERT_EQ(got.frames.size(), 1u);
    EXPECT_EQ(got.frames[0].data.size(), 12u);
    EXPECT_EQ(got.frames[0].data, payload);
    EXPECT_TRUE(got.frames[0].edl);
    EXPECT_EQ(got.frames[0].dlc, 15);
}

TEST(Decode, RemoteFrameIsSkipped) {
    // one remote record and no data or error records -> frames empty, error empty
    MdfBuilder builder;
    builder.add_remote_group(1);
    builder.add_remote_record(1);
    auto file = builder.build();
    auto got = decode_mf4(file.data(), file.size());
    ASSERT_TRUE(got.error.empty()) << got.error;
    EXPECT_TRUE(got.frames.empty());
}

TEST(Decode, StableSortKeepsDataSectionOrderOnTies) {
    // two classic frames, same timestamp, channel, id, and data, scan order A then B
    // output order A then B. Identity is the vector order of two equal frames.
    MdfBuilder builder;
    builder.add_classic_group(1, 1);
    builder.add_classic_record(1, 5, 0x10, 1, {0xab});
    builder.add_classic_record(1, 5, 0x10, 1, {0xab});
    auto file = builder.build();
    auto got = decode_mf4(file.data(), file.size());
    ASSERT_TRUE(got.error.empty()) << got.error;
    ASSERT_EQ(got.frames.size(), 2u);
    const auto& a = got.frames[0];
    const auto& b = got.frames[1];
    EXPECT_EQ(a.channel, "CAN1");
    EXPECT_EQ(a.timestamp_us, 5);
    EXPECT_EQ(a.id, 0x10u);
    EXPECT_EQ(a.dlc, 1);
    EXPECT_EQ(a.data, std::vector<uint8_t>({0xab}));
    EXPECT_FALSE(a.ide);
    EXPECT_FALSE(a.rtr);
    EXPECT_FALSE(a.err);
    EXPECT_FALSE(a.edl);
    EXPECT_FALSE(a.brs);
    EXPECT_FALSE(a.esi);
    EXPECT_FALSE(a.tx);
    EXPECT_EQ(b.channel, a.channel);
    EXPECT_EQ(b.timestamp_us, a.timestamp_us);
    EXPECT_EQ(b.id, a.id);
    EXPECT_EQ(b.dlc, a.dlc);
    EXPECT_EQ(b.data, a.data);
    EXPECT_EQ(b.ide, a.ide);
    EXPECT_EQ(b.rtr, a.rtr);
    EXPECT_EQ(b.err, a.err);
    EXPECT_EQ(b.edl, a.edl);
    EXPECT_EQ(b.brs, a.brs);
    EXPECT_EQ(b.esi, a.esi);
    EXPECT_EQ(b.tx, a.tx);
}

TEST(Decode, NanosecondScaleDividesSum) {
    // timestamp CC slope is the float64 of 1e-9, raw 1500, start_ns 2500
    // timestamp_us == (2500 + 1500) / 1000 == 4
    MdfBuilder builder;
    builder.start_ns = 2500;
    builder.add_classic_group(1, 1, 1e-9);
    builder.add_classic_record(1, 1500, 1, 0, {});
    auto file = builder.build();
    auto got = decode_mf4(file.data(), file.size());
    ASSERT_TRUE(got.error.empty()) << got.error;
    ASSERT_EQ(got.frames.size(), 1u);
    EXPECT_EQ(got.frames[0].timestamp_us, 4);
}

TEST(Decode, RejectsFinalizedId) {
    // first bytes "MDF     " -> non-empty error
    auto file = build_classic_rx(0, 0, 1, 1, 0, {});
    std::memcpy(file.data(), "MDF     ", 8);
    auto got = decode_mf4(file.data(), file.size());
    EXPECT_FALSE(got.error.empty());
}

TEST(Decode, RejectsLocalTime) {
    // hd_time_flags bit 0 set -> non-empty error
    MdfBuilder builder;
    builder.time_flags = 0x01;
    builder.add_classic_group(1, 1);
    builder.add_classic_record(1, 0, 1, 0, {});
    auto file = builder.build();
    auto got = decode_mf4(file.data(), file.size());
    EXPECT_FALSE(got.error.empty());
}

TEST(Decode, RejectsUnknownErrorType) {
    // ErrorType 6 -> non-empty error, zero frames
    MdfBuilder builder;
    builder.add_error_group(2, 1);
    builder.add_error_record(2, 0, 6);
    auto file = builder.build();
    auto got = decode_mf4(file.data(), file.size());
    EXPECT_FALSE(got.error.empty());
    EXPECT_TRUE(got.frames.empty());
}

TEST(Decode, RejectsTruncatedRecord) {
    // DT payload is a single 0x01 byte -> non-empty error
    MdfBuilder builder;
    builder.add_classic_group(1, 1);
    builder.set_dt_payload({0x01});
    auto file = builder.build();
    auto got = decode_mf4(file.data(), file.size());
    EXPECT_FALSE(got.error.empty());
}

TEST(Decode, ChannelTenSortsBeforeChannelTwo) {
    // CAN2 timestamp 0 and CAN10 timestamp 0, different ids
    // output channel order is CAN10 then CAN2
    MdfBuilder builder;
    builder.add_classic_group(1, 2);
    builder.add_classic_group(2, 10);
    builder.add_classic_record(1, 0, 0x11, 0, {});
    builder.add_classic_record(2, 0, 0x22, 0, {});
    auto file = builder.build();
    auto got = decode_mf4(file.data(), file.size());
    ASSERT_TRUE(got.error.empty()) << got.error;
    ASSERT_EQ(got.frames.size(), 2u);
    EXPECT_EQ(got.frames[0].channel, "CAN10");
    EXPECT_EQ(got.frames[0].id, 0x22u);
    EXPECT_EQ(got.frames[1].channel, "CAN2");
    EXPECT_EQ(got.frames[1].id, 0x11u);
}
