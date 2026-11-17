#ifndef CYPHAL_REASSEMBLE_TRANSFER_H
#define CYPHAL_REASSEMBLE_TRANSFER_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// One reassembled Cyphal transfer. Mirrors sc_schema.TransferSchema minus `channel`.
struct Transfer {
    int64_t timestamp_us = 0;              // microseconds since UNIX epoch, UTC (first frame)
    std::string type;                      // "Message" | "Request" | "Response"
    int16_t id = 0;                        // subject-id or service-id
    std::optional<uint8_t> source;         // origin node-id; empty if anonymous
    std::optional<uint8_t> dest;           // destination node-id; empty for messages
    uint8_t priority = 0;                  // 0..7
    uint8_t transfer_id = 0;               // 0..31
    std::vector<uint8_t> payload;
    int32_t length = 0;                    // payload.size()
};

#endif
