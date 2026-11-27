#include "decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

DecodeResult fail(const char* error) {
    DecodeResult result;
    result.error = error;
    return result;
}

uint16_t ru16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

uint32_t ru32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t ru64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

double rf64(const uint8_t* p) {
    uint64_t u = ru64(p);
    double v = 0;
    std::memcpy(&v, &u, sizeof v);
    return v;
}

struct BlockRef {
    size_t off = 0;
    uint64_t length = 0;
    uint64_t links = 0;
};

bool parse_block(const uint8_t* data, size_t size, size_t off, BlockRef& block) {
    if (off > size || size - off < 24) return false;
    if (data[off] != '#' || data[off + 1] != '#') return false;
    uint64_t length = ru64(data + off + 8);
    uint64_t links = ru64(data + off + 16);
    if (length < 24 || links > (length - 24) / 8 || length > size - off) return false;
    block.off = off;
    block.length = length;
    block.links = links;
    return true;
}

uint64_t link_at(const uint8_t* data, const BlockRef& block, unsigned index) {
    if (index >= block.links) return 0;
    return ru64(data + block.off + 24 + 8 * static_cast<size_t>(index));
}

const uint8_t* data_at(const uint8_t* data, const BlockRef& block) {
    return data + block.off + 24 + 8 * static_cast<size_t>(block.links);
}

size_t data_size(const BlockRef& block) { return static_cast<size_t>(block.length - 24 - 8 * block.links); }

bool known_block(const std::vector<size_t>& blocks, uint64_t off) {
    if (off == 0) return true;
    return std::find(blocks.begin(), blocks.end(), static_cast<size_t>(off)) != blocks.end();
}

std::string tx_text(const uint8_t* data, size_t size, uint64_t off, const char** error) {
    if (off == 0) return {};
    BlockRef block;
    if (!parse_block(data, size, static_cast<size_t>(off), block) || std::memcmp(data + off, "##TX", 4) != 0) {
        *error = "MF4 channel name is not a text block.";
        return {};
    }
    size_t text = block.off + 24 + 8 * static_cast<size_t>(block.links);
    size_t end = block.off + static_cast<size_t>(block.length);
    std::string out;
    for (size_t i = text; i < end; ++i) {
        if (data[i] == 0) return out;
        out.push_back(static_cast<char>(data[i]));
    }
    *error = "MF4 channel name is not terminated.";
    return {};
}

struct Channel {
    std::string name;
    uint8_t type = 0;
    uint8_t bit_offset = 0;
    uint32_t byte_offset = 0;
    uint32_t bit_count = 0;
    uint64_t cc = 0;
    uint64_t data_link = 0;
    std::vector<Channel> children;
};

std::string leaf_name(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) return name;
    return name.substr(dot + 1);
}

bool parse_channels(const uint8_t* data, size_t size, const std::vector<size_t>& blocks, uint64_t start,
                    std::vector<Channel>& out, const char** error, int depth) {
    if (depth > 8) {
        *error = "MF4 channel composition is too deep.";
        return false;
    }
    std::vector<uint64_t> seen;
    uint64_t off = start;
    while (off != 0) {
        if (std::find(seen.begin(), seen.end(), off) != seen.end() || seen.size() > 10000) {
            *error = "MF4 channel list is cyclic.";
            return false;
        }
        seen.push_back(off);
        if (!known_block(blocks, off)) {
            *error = "MF4 link is outside the block chain.";
            return false;
        }
        BlockRef block;
        if (!parse_block(data, size, static_cast<size_t>(off), block) || std::memcmp(data + off, "##CN", 4) != 0) {
            *error = "MF4 channel block is invalid.";
            return false;
        }
        if (data_size(block) < 12) {
            *error = "MF4 channel block is truncated.";
            return false;
        }
        const uint8_t* d = data_at(data, block);
        Channel ch;
        ch.type = d[0];
        ch.bit_offset = d[3];
        ch.byte_offset = ru32(d + 4);
        ch.bit_count = ru32(d + 8);
        ch.cc = link_at(data, block, 4);
        ch.data_link = link_at(data, block, 5);
        ch.name = tx_text(data, size, link_at(data, block, 2), error);
        if (*error != nullptr) return false;
        uint64_t composition = link_at(data, block, 1);
        if (composition != 0) {
            if (!parse_channels(data, size, blocks, composition, ch.children, error, depth + 1)) return false;
        }
        uint64_t next = link_at(data, block, 0);
        out.push_back(std::move(ch));
        off = next;
    }
    return true;
}

const Channel* find_name(const std::vector<Channel>& channels, const char* name) {
    for (const auto& ch : channels) {
        if (ch.name == name) return &ch;
        if (const Channel* nested = find_name(ch.children, name)) return nested;
    }
    return nullptr;
}

const Channel* find_type(const std::vector<Channel>& channels, uint8_t type) {
    for (const auto& ch : channels) {
        if (ch.type == type) return &ch;
        if (const Channel* nested = find_type(ch.children, type)) return nested;
    }
    return nullptr;
}

const Channel* find_leaf(const std::vector<Channel>& channels, const char* leaf) {
    for (const auto& ch : channels) {
        if (leaf_name(ch.name) == leaf) return &ch;
        if (const Channel* nested = find_leaf(ch.children, leaf)) return nested;
    }
    return nullptr;
}

struct Group {
    size_t file_offset = 0;
    uint64_t record_id = 0;
    uint16_t flags = 0;
    uint32_t data_bytes = 0;
    bool data_frame = false;
    bool error_frame = false;
    bool vlsd = false;
    std::vector<Channel> channels;
};

bool read_linear(const uint8_t* data, size_t size, const std::vector<size_t>& blocks, uint64_t cc, double& v0,
                 double& v1, const char** error) {
    if (cc == 0 || !known_block(blocks, cc)) {
        *error = "MF4 channel has no conversion.";
        return false;
    }
    BlockRef block;
    if (!parse_block(data, size, static_cast<size_t>(cc), block) || std::memcmp(data + cc, "##CC", 4) != 0) {
        *error = "MF4 conversion block is invalid.";
        return false;
    }
    if (data_size(block) < 40) {
        *error = "MF4 conversion block is truncated.";
        return false;
    }
    const uint8_t* d = data_at(data, block);
    uint16_t val_count = ru16(d + 6);
    if (d[0] != 1 || val_count < 2) {
        *error = "MF4 channel conversion is not linear.";
        return false;
    }
    v0 = rf64(d + 24);
    v1 = rf64(d + 32);
    return true;
}

bool virtual_int(const uint8_t* data, size_t size, const std::vector<size_t>& blocks, const Channel& ch, int64_t lo,
                 int64_t hi, int64_t& out, const char** error) {
    if (ch.type != 6) {
        *error = "MF4 virtual channel has the wrong type.";
        return false;
    }
    double v0 = 0;
    double v1 = 0;
    if (!read_linear(data, size, blocks, ch.cc, v0, v1, error)) return false;
    if (v1 != 0.0) {
        *error = "MF4 virtual channel slope is not 0.";
        return false;
    }
    if (!std::isfinite(v0) || v0 != std::trunc(v0) || v0 < static_cast<double>(lo) || v0 > static_cast<double>(hi)) {
        *error = "MF4 virtual channel value is out of range.";
        return false;
    }
    out = static_cast<int64_t>(v0);
    return true;
}

bool read_bits(const uint8_t* rec, size_t rec_len, uint32_t byte_offset, uint32_t bit_offset, uint32_t nbits,
               uint64_t& out, const char** error) {
    if (nbits > 64) {
        *error = "MF4 channel is wider than 64 bits.";
        return false;
    }
    if (nbits == 0) {
        out = 0;
        return true;
    }
    uint64_t value = 0;
    uint32_t got = 0;
    uint64_t byte = uint64_t(byte_offset) + uint64_t(bit_offset) / 8;
    unsigned shift = static_cast<unsigned>(bit_offset % 8);
    while (got < nbits) {
        if (byte >= rec_len) {
            *error = "MF4 channel extends past the record.";
            return false;
        }
        unsigned take = 8u - shift;
        if (take > nbits - got) take = nbits - got;
        const uint64_t mask = (uint64_t{1} << take) - 1u;
        value |= ((uint64_t(rec[byte]) >> shift) & mask) << got;
        got += take;
        shift = 0;
        ++byte;
    }
    out = value;
    return true;
}

// Slope is the float64 of the C++ literals 1e-6 and 1e-9. Any other value fails.
const char* read_timestamp(const uint8_t* data, size_t size, const std::vector<size_t>& blocks, const Channel& timestamp,
                           const uint8_t* rec, size_t rec_len, uint64_t start_time, int64_t& out) {
    const char* error = nullptr;
    uint64_t raw = 0;
    if (!read_bits(rec, rec_len, timestamp.byte_offset, timestamp.bit_offset, timestamp.bit_count, raw, &error)) {
        return error;
    }
    double time_offset = 0;
    double time_slope = 0;
    if (!read_linear(data, size, blocks, timestamp.cc, time_offset, time_slope, &error)) return error;
    // The linear offset on the session file is 0.
    (void)time_offset;
    if (time_slope == 1e-6) {
        uint64_t base = start_time / 1000;
        if (base > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
            raw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - base) {
            return "MF4 timestamp is out of range.";
        }
        out = static_cast<int64_t>(base) + static_cast<int64_t>(raw);
        return nullptr;
    }
    if (time_slope == 1e-9) {
        if (raw > std::numeric_limits<uint64_t>::max() - start_time) return "MF4 timestamp is out of range.";
        uint64_t micros = (start_time + raw) / 1000;
        if (micros > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return "MF4 timestamp is out of range.";
        }
        out = static_cast<int64_t>(micros);
        return nullptr;
    }
    return "MF4 time scale is not 1e-6 or 1e-9.";
}

struct Fields {
    const Channel* bus = nullptr;
    const Channel* id = nullptr;
    const Channel* ide = nullptr;
    const Channel* dlc = nullptr;
    const Channel* length = nullptr;
    const Channel* bytes = nullptr;
    const Channel* dir = nullptr;
    const Channel* edl = nullptr;
    const Channel* brs = nullptr;
    const Channel* esi = nullptr;
};

void collect_fields(const std::vector<Channel>& channels, Fields& fields) {
    for (const auto& ch : channels) {
        const std::string leaf = leaf_name(ch.name);
        if (leaf == "BusChannel") fields.bus = &ch;
        else if (leaf == "ID") fields.id = &ch;
        else if (leaf == "IDE") fields.ide = &ch;
        else if (leaf == "DLC") fields.dlc = &ch;
        else if (leaf == "DataLength") fields.length = &ch;
        else if (leaf == "DataBytes") fields.bytes = &ch;
        else if (leaf == "Dir") fields.dir = &ch;
        else if (leaf == "EDL") fields.edl = &ch;
        else if (leaf == "BRS") fields.brs = &ch;
        else if (leaf == "ESI") fields.esi = &ch;
        collect_fields(ch.children, fields);
    }
}

bool read_flag(const uint8_t* data, size_t size, const std::vector<size_t>& blocks, const uint8_t* rec,
               size_t rec_len, const Channel& ch, bool& out, const char** error) {
    if (ch.type == 6) {
        int64_t value = 0;
        if (!virtual_int(data, size, blocks, ch, 0, 1, value, error)) return false;
        out = value == 1;
        return true;
    }
    if (ch.type != 0) {
        *error = "MF4 channel type is unsupported.";
        return false;
    }
    uint64_t bits = 0;
    if (!read_bits(rec, rec_len, ch.byte_offset, ch.bit_offset, ch.bit_count, bits, error)) return false;
    if (bits > 1) {
        *error = "MF4 flag value is out of range.";
        return false;
    }
    out = bits == 1;
    return true;
}

struct BitField {
    uint32_t byte_offset = 0;
    uint32_t bit_offset = 0;
    uint32_t bit_count = 0;
};

struct FlagPlan {
    bool present = false;
    bool constant = false;
    bool value = false;
    BitField bits;
};

// Channel layout and virtual constants are the same for every row of a group.
// Resolving them once removes per-row name walks and "CAN" + std::to_string.
struct DataPlan {
    std::string channel;
    BitField timestamp;
    bool nanoseconds = false;
    int64_t base_us = 0;
    uint64_t start_time = 0;
    FlagPlan ide;
    FlagPlan tx;
    FlagPlan edl;
    FlagPlan brs;
    FlagPlan esi;
    BitField id;
    BitField dlc;
    BitField length;
    bool vlsd = false;
    size_t vlsd_group = 0;
    BitField vlsd_offset;
    uint32_t bytes_offset = 0;
    uint32_t bytes_size = 0;
};

const char* bind_flag(const uint8_t* data, size_t size, const std::vector<size_t>& blocks, const Channel* channel,
                      bool required, FlagPlan& out) {
    if (channel == nullptr) {
        if (required) return "MF4 data frame is missing a channel.";
        out.present = false;
        return nullptr;
    }
    out.present = true;
    if (channel->type == 6) {
        const char* error = nullptr;
        bool value = false;
        if (!read_flag(data, size, blocks, nullptr, 0, *channel, value, &error)) return error;
        out.constant = true;
        out.value = value;
        return nullptr;
    }
    if (channel->type != 0) return "MF4 channel type is unsupported.";
    out.constant = false;
    out.bits.byte_offset = channel->byte_offset;
    out.bits.bit_offset = channel->bit_offset;
    out.bits.bit_count = channel->bit_count;
    return nullptr;
}

const char* apply_flag(const FlagPlan& flag, const uint8_t* rec, size_t rec_len, bool& out) {
    if (!flag.present) {
        out = false;
        return nullptr;
    }
    if (flag.constant) {
        out = flag.value;
        return nullptr;
    }
    const char* error = nullptr;
    uint64_t bits = 0;
    if (!read_bits(rec, rec_len, flag.bits.byte_offset, flag.bits.bit_offset, flag.bits.bit_count, bits, &error)) {
        return error;
    }
    if (bits > 1) return "MF4 flag value is out of range.";
    out = bits == 1;
    return nullptr;
}

const char* build_data_plan(const uint8_t* data, size_t size, const std::vector<size_t>& blocks,
                            const std::vector<Group>& groups, const Group& group, uint64_t start_time, DataPlan& plan) {
    const Channel* root = find_name(group.channels, "CAN_DataFrame");
    const Channel* timestamp = find_type(group.channels, 2);
    if (root == nullptr || timestamp == nullptr) return "MF4 data frame is missing a channel.";
    Fields fields;
    collect_fields(root->children, fields);
    if (fields.bus == nullptr || fields.id == nullptr || fields.ide == nullptr || fields.dlc == nullptr ||
        fields.length == nullptr || fields.bytes == nullptr || fields.dir == nullptr) {
        return "MF4 data frame is missing a channel.";
    }

    const char* error = nullptr;
    double time_offset = 0;
    double time_slope = 0;
    if (!read_linear(data, size, blocks, timestamp->cc, time_offset, time_slope, &error)) return error;
    (void)time_offset;
    plan.timestamp.byte_offset = timestamp->byte_offset;
    plan.timestamp.bit_offset = timestamp->bit_offset;
    plan.timestamp.bit_count = timestamp->bit_count;
    if (time_slope == 1e-6) {
        uint64_t base = start_time / 1000;
        if (base > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return "MF4 timestamp is out of range.";
        }
        plan.nanoseconds = false;
        plan.base_us = static_cast<int64_t>(base);
    } else if (time_slope == 1e-9) {
        plan.nanoseconds = true;
        plan.start_time = start_time;
    } else {
        return "MF4 time scale is not 1e-6 or 1e-9.";
    }

    int64_t bus = 0;
    if (!virtual_int(data, size, blocks, *fields.bus, 0, 2147483647, bus, &error)) return error;
    plan.channel = "CAN" + std::to_string(bus);

    if (const char* flag_error = bind_flag(data, size, blocks, fields.ide, true, plan.ide)) return flag_error;
    if (const char* flag_error = bind_flag(data, size, blocks, fields.dir, true, plan.tx)) return flag_error;
    if (const char* flag_error = bind_flag(data, size, blocks, fields.edl, false, plan.edl)) return flag_error;
    if (const char* flag_error = bind_flag(data, size, blocks, fields.brs, false, plan.brs)) return flag_error;
    if (const char* flag_error = bind_flag(data, size, blocks, fields.esi, false, plan.esi)) return flag_error;

    if (fields.id->type != 0 || fields.dlc->type != 0 || fields.length->type != 0) {
        return "MF4 channel type is unsupported.";
    }
    plan.id = BitField{fields.id->byte_offset, fields.id->bit_offset, fields.id->bit_count};
    plan.dlc = BitField{fields.dlc->byte_offset, fields.dlc->bit_offset, fields.dlc->bit_count};
    plan.length = BitField{fields.length->byte_offset, fields.length->bit_offset, fields.length->bit_count};

    if (fields.bytes->type == 1) {
        plan.vlsd = true;
        plan.vlsd_offset =
            BitField{fields.bytes->byte_offset, fields.bytes->bit_offset, fields.bytes->bit_count};
        plan.vlsd_group = groups.size();
        for (size_t i = 0; i < groups.size(); ++i) {
            if (groups[i].vlsd && groups[i].file_offset == fields.bytes->data_link) {
                plan.vlsd_group = i;
                break;
            }
        }
        if (plan.vlsd_group == groups.size()) return "MF4 variable length channel has no data group.";
        return nullptr;
    }
    if (fields.bytes->type != 0) return "MF4 variable length payload is not supported.";
    if (fields.bytes->bit_offset != 0 || fields.bytes->bit_count % 8 != 0) {
        return "MF4 data bytes are not a byte array.";
    }
    uint32_t nbytes = fields.bytes->bit_count / 8;
    if (nbytes > group.data_bytes || fields.bytes->byte_offset > group.data_bytes - nbytes) {
        return "MF4 channel extends past the record.";
    }
    plan.vlsd = false;
    plan.bytes_offset = fields.bytes->byte_offset;
    plan.bytes_size = nbytes;
    return nullptr;
}

const char* decode_prepared_frame(const DataPlan& plan, const std::vector<std::vector<uint8_t>>& vlsd_bytes,
                                  const uint8_t* rec, size_t rec_len, Frame& frame) {
    const char* error = nullptr;
    uint64_t raw = 0;
    if (!read_bits(rec, rec_len, plan.timestamp.byte_offset, plan.timestamp.bit_offset, plan.timestamp.bit_count, raw,
                   &error)) {
        return error;
    }
    if (!plan.nanoseconds) {
        if (raw > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - static_cast<uint64_t>(plan.base_us)) {
            return "MF4 timestamp is out of range.";
        }
        frame.timestamp_us = plan.base_us + static_cast<int64_t>(raw);
    } else {
        if (raw > std::numeric_limits<uint64_t>::max() - plan.start_time) return "MF4 timestamp is out of range.";
        uint64_t micros = (plan.start_time + raw) / 1000;
        if (micros > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return "MF4 timestamp is out of range.";
        }
        frame.timestamp_us = static_cast<int64_t>(micros);
    }

    frame.channel = plan.channel;
    frame.rtr = false;
    frame.err = false;
    if (const char* flag_error = apply_flag(plan.ide, rec, rec_len, frame.ide)) return flag_error;
    if (const char* flag_error = apply_flag(plan.tx, rec, rec_len, frame.tx)) return flag_error;
    if (const char* flag_error = apply_flag(plan.edl, rec, rec_len, frame.edl)) return flag_error;
    if (const char* flag_error = apply_flag(plan.brs, rec, rec_len, frame.brs)) return flag_error;
    if (const char* flag_error = apply_flag(plan.esi, rec, rec_len, frame.esi)) return flag_error;

    uint64_t id = 0;
    if (!read_bits(rec, rec_len, plan.id.byte_offset, plan.id.bit_offset, plan.id.bit_count, id, &error)) return error;
    if (id > 0xffffffffu) return "MF4 id is out of range.";
    frame.id = static_cast<uint32_t>(id);

    uint64_t dlc = 0;
    if (!read_bits(rec, rec_len, plan.dlc.byte_offset, plan.dlc.bit_offset, plan.dlc.bit_count, dlc, &error)) {
        return error;
    }
    if (dlc > 255) return "MF4 dlc is out of range.";
    frame.dlc = static_cast<uint8_t>(dlc);

    uint64_t data_length = 0;
    if (!read_bits(rec, rec_len, plan.length.byte_offset, plan.length.bit_offset, plan.length.bit_count, data_length,
                   &error)) {
        return error;
    }
    if (plan.vlsd) {
        // Byte offset into the VLSD group's records. DataLength does not trim the payload.
        (void)data_length;
        uint64_t offset = 0;
        if (!read_bits(rec, rec_len, plan.vlsd_offset.byte_offset, plan.vlsd_offset.bit_offset,
                       plan.vlsd_offset.bit_count, offset, &error)) {
            return error;
        }
        const std::vector<uint8_t>& blob = vlsd_bytes[plan.vlsd_group];
        if (offset > blob.size() || blob.size() - static_cast<size_t>(offset) < 4) {
            return "MF4 variable length offset is out of range.";
        }
        size_t at = static_cast<size_t>(offset);
        uint32_t n = ru32(blob.data() + at);
        if (static_cast<size_t>(n) > blob.size() - at - 4) return "MF4 variable length payload does not fit.";
        frame.data.assign(blob.data() + at + 4, n);
        return nullptr;
    }
    if (data_length > plan.bytes_size) return "MF4 data length does not fit.";
    frame.data.assign(rec + plan.bytes_offset, static_cast<size_t>(data_length));
    return nullptr;
}

struct SocketcanError {
    uint32_t id;
    uint8_t bytes[8];
};

// SocketCAN error-class map from the design spec / mf4.py.
const SocketcanError kSocketcanErrors[] = {
    {0x8, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x8, {0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x8, {0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x8, {0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x8, {0x00, 0x00, 0x01, 0x08, 0x00, 0x00, 0x00, 0x00}},
    {0x20, {0x00, 0x00, 0x01, 0x19, 0x00, 0x00, 0x00, 0x00}},
};

const char* decode_error_frame(const uint8_t* data, size_t size, const std::vector<size_t>& blocks, const Group& group,
                               const uint8_t* rec, uint64_t start_time, Frame& frame) {
    const Channel* root = find_name(group.channels, "CAN_ErrorFrame");
    const Channel* timestamp = find_type(group.channels, 2);
    const Channel* bus = find_leaf(group.channels, "BusChannel");
    const Channel* error_type = find_leaf(group.channels, "ErrorType");
    if (root == nullptr || timestamp == nullptr || bus == nullptr || error_type == nullptr) {
        return "MF4 error frame is missing a channel.";
    }

    const char* error =
        read_timestamp(data, size, blocks, *timestamp, rec, group.data_bytes, start_time, frame.timestamp_us);
    if (error != nullptr) return error;

    int64_t bus_value = 0;
    if (!virtual_int(data, size, blocks, *bus, 0, 2147483647, bus_value, &error)) return error;
    frame.channel = "CAN" + std::to_string(bus_value);

    if (error_type->type != 0) return "MF4 channel type is unsupported.";
    uint64_t kind = 0;
    if (!read_bits(rec, group.data_bytes, error_type->byte_offset, error_type->bit_offset, error_type->bit_count, kind,
                   &error)) {
        return error;
    }
    if (kind > 5) return "MF4 error type is out of range.";

    for (const char* name : {"Dir", "IDE", "EDL", "BRS", "ESI"}) {
        const Channel* ch = find_leaf(group.channels, name);
        if (ch == nullptr) continue;
        bool ignored = false;
        if (!read_flag(data, size, blocks, rec, group.data_bytes, *ch, ignored, &error)) return error;
    }

    const SocketcanError& row = kSocketcanErrors[static_cast<size_t>(kind)];
    frame.id = row.id;
    frame.dlc = 8;
    frame.data.assign(row.bytes, 8);
    frame.ide = false;
    frame.rtr = false;
    frame.err = true;
    frame.edl = false;
    frame.brs = false;
    frame.esi = false;
    frame.tx = false;
    return nullptr;
}

struct TaggedFrame {
    Frame frame;
    size_t index = 0;
};

}  // namespace

DecodeResult decode_mf4(const uint8_t* data, size_t size) {
    if (size < 64 || data == nullptr || std::memcmp(data, "UnFinMF 4.11    ", 16) != 0) {
        return fail("MF4 id is not UnFinMF 4.11.");
    }

    std::vector<size_t> blocks;
    size_t offset = 64;
    while (offset + 24 <= size) {
        if (data[offset] != '#' || data[offset + 1] != '#') break;
        uint64_t length = ru64(data + offset + 8);
        uint64_t links = ru64(data + offset + 16);
        if (length < 24 || links > (length - 24) / 8 || length > size - offset) break;
        blocks.push_back(offset);
        offset = (offset + static_cast<size_t>(length) + 7) & ~size_t{7};
    }
    if (blocks.empty() || blocks.front() != 64 || std::memcmp(data + 64, "##HD", 4) != 0) {
        return fail("MF4 header block is missing.");
    }

    BlockRef hd;
    if (!parse_block(data, size, 64, hd) || data_size(hd) < 13 || hd.links < 1) {
        return fail("MF4 header block is truncated.");
    }
    uint64_t dg_off = link_at(data, hd, 0);
    if (dg_off == 0) return fail("MF4 header has no data group.");
    if (!known_block(blocks, dg_off)) return fail("MF4 link is outside the block chain.");
    const uint8_t* hd_data = data_at(data, hd);
    uint64_t start_time = ru64(hd_data);
    if ((hd_data[12] & 0x01) != 0) return fail("MF4 header time is local.");

    BlockRef dg;
    if (!parse_block(data, size, static_cast<size_t>(dg_off), dg) || std::memcmp(data + dg_off, "##DG", 4) != 0) {
        return fail("MF4 data group block is missing.");
    }
    if (link_at(data, dg, 0) != 0) return fail("MF4 has more than one data group.");
    if (dg.links < 3 || data_size(dg) < 1) return fail("MF4 data group has no data block.");
    if (data_at(data, dg)[0] != 1) return fail("MF4 record id size is not 1.");
    uint64_t dt_off = link_at(data, dg, 2);
    if (dt_off == 0 || !known_block(blocks, dt_off)) return fail("MF4 data group has no data block.");
    BlockRef dt;
    if (!parse_block(data, size, static_cast<size_t>(dt_off), dt) || std::memcmp(data + dt_off, "##DT", 4) != 0 ||
        dt.length != 24) {
        return fail("MF4 data block is not one raw DT.");
    }

    std::vector<Group> groups;
    uint64_t cg_off = link_at(data, dg, 1);
    std::vector<uint64_t> seen_cg;
    const char* error = nullptr;
    while (cg_off != 0) {
        if (std::find(seen_cg.begin(), seen_cg.end(), cg_off) != seen_cg.end()) {
            return fail("MF4 channel group list is cyclic.");
        }
        seen_cg.push_back(cg_off);
        if (!known_block(blocks, cg_off)) return fail("MF4 link is outside the block chain.");
        BlockRef cg;
        if (!parse_block(data, size, static_cast<size_t>(cg_off), cg) || std::memcmp(data + cg_off, "##CG", 4) != 0) {
            return fail("MF4 channel group block is invalid.");
        }
        if (data_size(cg) < 32) return fail("MF4 channel group block is truncated.");
        const uint8_t* cg_data = data_at(data, cg);
        Group group;
        group.file_offset = static_cast<size_t>(cg_off);
        group.record_id = ru64(cg_data);
        group.flags = ru16(cg_data + 16);
        group.data_bytes = ru32(cg_data + 24);
        if (!parse_channels(data, size, blocks, link_at(data, cg, 1), group.channels, &error, 0)) {
            return fail(error != nullptr ? error : "MF4 channel block is invalid.");
        }
        group.vlsd = (group.flags & 0x0001u) != 0;
        if (!group.vlsd) {
            group.data_frame = find_name(group.channels, "CAN_DataFrame") != nullptr;
            group.error_frame = !group.data_frame && find_name(group.channels, "CAN_ErrorFrame") != nullptr;
        }
        for (const auto& prev : groups) {
            if (prev.record_id == group.record_id) return fail("MF4 record id is repeated.");
        }
        uint64_t next = link_at(data, cg, 0);
        groups.push_back(std::move(group));
        cg_off = next;
    }

    struct Pending {
        size_t group_index = 0;
        size_t rec = 0;
        size_t index = 0;
    };

    const Group* group_by_id[256] = {};
    size_t group_index_by_id[256] = {};
    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].record_id > 255) continue;
        auto id = static_cast<uint8_t>(groups[i].record_id);
        group_by_id[id] = &groups[i];
        group_index_by_id[id] = i;
    }

    std::vector<std::vector<uint8_t>> vlsd_bytes(groups.size());
    std::vector<Pending> pending;
    pending.reserve(size / 8);
    size_t pos = static_cast<size_t>(dt_off) + 24;
    size_t scan = 0;
    while (pos < size) {
        uint8_t record_id = data[pos++];
        const Group* group = group_by_id[record_id];
        if (group == nullptr) return fail("MF4 record id is unknown.");
        size_t group_index = group_index_by_id[record_id];
        if (group->vlsd) {
            if (size - pos < 4) return fail("MF4 record is truncated.");
            uint32_t n = ru32(data + pos);
            if (static_cast<size_t>(n) > size - pos - 4) return fail("MF4 record is truncated.");
            size_t nbytes = 4 + static_cast<size_t>(n);
            vlsd_bytes[group_index].insert(vlsd_bytes[group_index].end(), data + pos, data + pos + nbytes);
            pos += nbytes;
            continue;
        }
        if (group->data_bytes > size - pos) return fail("MF4 record is truncated.");
        if (group->data_frame || group->error_frame) {
            pending.push_back(Pending{group_index, pos, scan});
            ++scan;
        }
        pos += group->data_bytes;
    }

    std::vector<std::unique_ptr<DataPlan>> plans(groups.size());
    std::vector<TaggedFrame> rows;
    rows.reserve(pending.size());
    for (const Pending& item : pending) {
        const Group& group = groups[item.group_index];
        TaggedFrame tagged;
        tagged.index = item.index;
        const char* row_error = nullptr;
        if (group.data_frame) {
            if (!plans[item.group_index]) {
                auto plan = std::make_unique<DataPlan>();
                row_error = build_data_plan(data, size, blocks, groups, group, start_time, *plan);
                if (row_error == nullptr) plans[item.group_index] = std::move(plan);
            }
            if (row_error == nullptr) {
                row_error = decode_prepared_frame(*plans[item.group_index], vlsd_bytes, data + item.rec,
                                                  group.data_bytes, tagged.frame);
            }
        } else {
            row_error = decode_error_frame(data, size, blocks, group, data + item.rec, start_time, tagged.frame);
        }
        if (row_error != nullptr) return fail(row_error);
        rows.push_back(std::move(tagged));
    }

    auto before = [](const TaggedFrame& a, const TaggedFrame& b) {
        if (a.frame.timestamp_us != b.frame.timestamp_us) return a.frame.timestamp_us < b.frame.timestamp_us;
        if (a.frame.channel != b.frame.channel) return a.frame.channel < b.frame.channel;
        if (a.frame.id != b.frame.id) return a.frame.id < b.frame.id;
        if (a.frame.data != b.frame.data) return a.frame.data < b.frame.data;
        return a.index < b.index;
    };
    if (!std::is_sorted(rows.begin(), rows.end(), before)) std::stable_sort(rows.begin(), rows.end(), before);

    DecodeResult result;
    result.frames.reserve(rows.size());
    for (auto& row : rows) result.frames.push_back(std::move(row.frame));
    return result;
}
