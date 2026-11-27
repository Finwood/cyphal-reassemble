#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Frame {
    std::string channel;
    int64_t timestamp_us = 0;
    uint32_t id = 0;
    uint8_t dlc = 0;
    bool ide = false;
    bool rtr = false;
    bool err = false;
    bool edl = false;
    bool brs = false;
    bool esi = false;
    bool tx = false;
    std::vector<uint8_t> data;
};

struct DecodeResult {
    std::string error;
    std::vector<Frame> frames;
};

DecodeResult decode_mf4(const uint8_t* data, size_t size);
