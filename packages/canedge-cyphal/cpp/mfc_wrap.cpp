#include <cstdint>
#include <iostream>
#include <vector>

#include "crc32.h"

extern "C" {
#include "heatshrink_encoder.h"
}

namespace {

std::vector<uint8_t> read_stdin() {
    std::vector<uint8_t> buf;
    char chunk[4096];
    while (std::cin.read(chunk, sizeof chunk) || std::cin.gcount() > 0) {
        auto n = static_cast<size_t>(std::cin.gcount());
        buf.insert(buf.end(), chunk, chunk + n);
    }
    return buf;
}

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

void write_header() {
    const char magic[] = "Generic File";
    std::cout.write(magic, 12);
    const uint8_t type[] = {0x01, 0x01, 0x22, 0x01};
    std::cout.write(reinterpret_cast<const char*>(type), 4);
    const uint8_t device[] = {0x06, 0xde, 0x1f, 0xe8};
    std::cout.write(reinterpret_cast<const char*>(device), 4);
    const uint8_t lookahead[] = {0x00, 0x00, 0x00, 0x04};
    std::cout.write(reinterpret_cast<const char*>(lookahead), 4);
    const uint8_t window[] = {0x00, 0x00, 0x01, 0x00};
    std::cout.write(reinterpret_cast<const char*>(window), 4);
}

}  // namespace

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    auto raw = read_stdin();
    auto compressed = encode_literals(raw);
    write_header();
    std::cout.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
    uint32_t crc = crc32_jamcrc(compressed.data(), compressed.size());
    uint8_t crc_be[] = {
        static_cast<uint8_t>(crc >> 24),
        static_cast<uint8_t>(crc >> 16),
        static_cast<uint8_t>(crc >> 8),
        static_cast<uint8_t>(crc),
    };
    std::cout.write(reinterpret_cast<const char*>(crc_be), 4);
    return 0;
}