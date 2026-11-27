#include "crc32.h"

namespace {

// Reflected CRC-32/JAMCRC table for poly 0xEDB88320.
uint32_t make_entry(uint32_t i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
        uint32_t mask = -(crc & 1u);
        crc = (crc >> 1) ^ (0xedb88320u & mask);
    }
    return crc;
}

struct Table {
    uint32_t v[256];
    Table() {
        for (uint32_t i = 0; i < 256; ++i) v[i] = make_entry(i);
    }
};

const Table& table() {
    static const Table t;
    return t;
}

}  // namespace

uint32_t crc32_jamcrc(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    const uint32_t* t = table().v;
    for (size_t i = 0; i < size; ++i) {
        crc = t[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    }
    return crc;
}
