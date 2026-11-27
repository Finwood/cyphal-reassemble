#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct MfcResult {
    std::string error;
    std::vector<uint8_t> bytes;
};

MfcResult unwrap_mfc(const uint8_t* data, size_t size);