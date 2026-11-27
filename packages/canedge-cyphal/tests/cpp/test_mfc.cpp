#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "crc32.h"
#include "mfc.h"

extern "C" {
#include "heatshrink_encoder.h"
}

namespace {

std::vector<uint8_t> encode_literals(const std::vector<uint8_t>& raw) {
    heatshrink_encoder* enc = heatshrink_encoder_alloc(8, 4);
    std::vector<uint8_t> out;
    size_t sunk = 0;
    while (sunk < raw.size()) {
        size_t n = 0;
        heatshrink_encoder_sink(enc, const_cast<uint8_t*>(raw.data() + sunk), raw.size() - sunk, &n);
        sunk += n;
        for (;;) {
            uint8_t buf[256];
            size_t got = 0;
            auto poll = heatshrink_encoder_poll(enc, buf, sizeof buf, &got);
            out.insert(out.end(), buf, buf + got);
            if (poll == HSER_POLL_EMPTY) break;
        }
    }
    for (;;) {
        auto fin = heatshrink_encoder_finish(enc);
        uint8_t buf[256];
        size_t got = 0;
        heatshrink_encoder_poll(enc, buf, sizeof buf, &got);
        out.insert(out.end(), buf, buf + got);
        if (fin == HSER_FINISH_DONE) break;
    }
    heatshrink_encoder_free(enc);
    return out;
}

std::vector<uint8_t> wrap(const std::vector<uint8_t>& compressed) {
    std::vector<uint8_t> file;
    const char magic[] = "Generic File";
    file.insert(file.end(), magic, magic + 12);
    file.push_back(0x01);
    file.push_back(0x01);
    file.push_back(0x22);
    file.push_back(0x01);
    file.insert(file.end(), {0x06, 0xde, 0x1f, 0xe8});
    file.insert(file.end(), {0x00, 0x00, 0x00, 0x04});  // lookahead 4
    file.insert(file.end(), {0x00, 0x00, 0x01, 0x00});  // window 256
    file.insert(file.end(), compressed.begin(), compressed.end());
    uint32_t crc = crc32_jamcrc(compressed.data(), compressed.size());
    file.push_back(static_cast<uint8_t>(crc >> 24));
    file.push_back(static_cast<uint8_t>(crc >> 16));
    file.push_back(static_cast<uint8_t>(crc >> 8));
    file.push_back(static_cast<uint8_t>(crc));
    return file;
}

}  // namespace

TEST(Mfc, PassesPlainBytesThrough) {
    const uint8_t raw[] = {'M', 'D', 'F', ' '};
    auto got = unwrap_mfc(raw, sizeof raw);
    EXPECT_TRUE(got.error.empty());
    EXPECT_EQ(got.bytes, std::vector<uint8_t>(raw, raw + sizeof raw));
}

TEST(Mfc, RoundTripsHeatshrink) {
    std::vector<uint8_t> raw(64);
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<uint8_t>(i * 3 + 1);
    auto file = wrap(encode_literals(raw));
    auto got = unwrap_mfc(file.data(), file.size());
    EXPECT_TRUE(got.error.empty()) << got.error;
    EXPECT_EQ(got.bytes, raw);
}

TEST(Mfc, RejectsBadChecksum) {
    std::vector<uint8_t> raw{'a'};
    auto file = wrap(encode_literals(raw));
    file.back() ^= 0xff;
    auto got = unwrap_mfc(file.data(), file.size());
    EXPECT_FALSE(got.error.empty());
}