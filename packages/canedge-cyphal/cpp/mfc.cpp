#include "mfc.h"

#include <cstring>

#include "crc32.h"

extern "C" {
#include "heatshrink_decoder.h"
}

namespace {

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void append_out(std::vector<uint8_t>& out, const uint8_t* buf, size_t got) {
    if (got == 0) return;
    size_t at = out.size();
    out.resize(at + got);
    std::memcpy(out.data() + at, buf, got);
}

std::string inflate(const uint8_t* in, size_t n, uint32_t lookahead, uint32_t window,
                    std::vector<uint8_t>& out) {
    if (window < 2 || (window & (window - 1)) != 0 || lookahead == 0 || lookahead > 15) {
        return "heatshrink window or lookahead is invalid";
    }
    uint8_t window_sz2 = 0;
    while ((uint32_t(1) << window_sz2) < window) ++window_sz2;
    // Large sink buffer: heatshrink only accepts up to input_buffer_size per sink.
    constexpr uint16_t kInputBuf = 65535;
    heatshrink_decoder* dec = heatshrink_decoder_alloc(kInputBuf, window_sz2, static_cast<uint8_t>(lookahead));
    if (dec == nullptr) return "heatshrink alloc failed";
    // CANedge MFCs on this corpus inflate ~1.5–2×; reserve to cut realloc copies.
    out.reserve(out.size() + n * 2);
    size_t sunk = 0;
    uint8_t buf[1 << 16];
    while (sunk < n) {
        size_t took = 0;
        if (heatshrink_decoder_sink(dec, const_cast<uint8_t*>(in + sunk), n - sunk, &took) < 0) {
            heatshrink_decoder_free(dec);
            return "heatshrink sink failed";
        }
        sunk += took;
        for (;;) {
            size_t got = 0;
            auto poll = heatshrink_decoder_poll(dec, buf, sizeof buf, &got);
            append_out(out, buf, got);
            if (poll == HSDR_POLL_EMPTY) break;
            if (poll != HSDR_POLL_MORE) {
                heatshrink_decoder_free(dec);
                return "heatshrink poll failed";
            }
        }
    }
    for (;;) {
        auto fin = heatshrink_decoder_finish(dec);
        size_t got = 0;
        auto poll = heatshrink_decoder_poll(dec, buf, sizeof buf, &got);
        append_out(out, buf, got);
        if (fin == HSDR_FINISH_DONE && poll == HSDR_POLL_EMPTY) break;
        if (fin < 0 || poll < 0) {
            heatshrink_decoder_free(dec);
            return "heatshrink finish failed";
        }
    }
    heatshrink_decoder_free(dec);
    return {};
}

}  // namespace

MfcResult unwrap_mfc(const uint8_t* data, size_t size) {
    MfcResult result;
    const char magic[] = "Generic File";
    if (size < 12 || std::memcmp(data, magic, 12) != 0) {
        result.bytes.assign(data, data + size);
        return result;
    }
    if (size < 32) {
        result.error = "MFC header is truncated";
        return result;
    }
    if (data[12] != 0x01 || data[13] != 0x01 || data[14] != 0x22 || data[15] != 0x01) {
        result.error = "MFC type is not heatshrink";
        return result;
    }
    uint32_t lookahead = be32(data + 20);
    uint32_t window = be32(data + 24);
    const uint8_t* compressed = data + 28;
    size_t compressed_size = size - 32;
    uint32_t expect = be32(data + size - 4);
    uint32_t got = crc32_jamcrc(compressed, compressed_size);
    if (expect != got) {
        result.error = "MFC checksum does not match";
        return result;
    }
    result.error = inflate(compressed, compressed_size, lookahead, window, result.bytes);
    if (!result.error.empty()) result.bytes.clear();
    return result;
}