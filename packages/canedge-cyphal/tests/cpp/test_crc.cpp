#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "crc32.h"

TEST(Crc32, JamcrcOf123456789) {
    const auto* p = reinterpret_cast<const uint8_t*>("123456789");
    EXPECT_EQ(crc32_jamcrc(p, 9), 0x340bc6d9u);
}

TEST(Crc32, EmptyIsAllOnes) {
    EXPECT_EQ(crc32_jamcrc(nullptr, 0), 0xffffffffu);
}