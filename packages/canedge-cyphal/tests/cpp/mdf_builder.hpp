#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// One unfinalized MF4 buffer: classic CANedge *_Rx group, one record.
// Block layout matches the session fixture's fixed-size data frame
// (16-byte record, virtual BusChannel / IDE / Dir, timestamp slope 1e-6).
inline std::vector<uint8_t> build_classic_rx(uint64_t start_ns, uint64_t raw_us, uint32_t bus,
                                             uint32_t can_id, uint8_t dlc, std::vector<uint8_t> payload) {
    std::vector<uint8_t> b;

    auto u16_at = [&](size_t off, uint16_t v) {
        b[off] = static_cast<uint8_t>(v);
        b[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    auto u32_at = [&](size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) b[off + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    auto u64_at = [&](size_t off, uint64_t v) {
        for (int i = 0; i < 8; ++i) b[off + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    auto f64_at = [&](size_t off, double v) { std::memcpy(b.data() + off, &v, sizeof v); };

    auto add_block = [&](const char* id, uint64_t nlinks, size_t data_bytes) {
        size_t aligned = (b.size() + 7) & ~size_t{7};
        b.resize(aligned, 0);
        size_t off = b.size();
        size_t len = 24 + 8 * static_cast<size_t>(nlinks) + data_bytes;
        b.resize(off + len, 0);
        std::memcpy(b.data() + off, id, 4);
        u64_at(off + 8, len);
        u64_at(off + 16, nlinks);
        return off;
    };
    auto link_at = [&](size_t block, int index, size_t target) {
        u64_at(block + 24 + 8 * static_cast<size_t>(index), target);
    };
    auto add_tx = [&](const std::string& text) {
        size_t off = add_block("##TX", 0, text.size() + 1);
        std::memcpy(b.data() + off + 24, text.data(), text.size());
        return off;
    };
    auto add_cc = [&](double offset, double slope) {
        size_t off = add_block("##CC", 4, 40);
        size_t d = off + 24 + 32;
        b[d] = 1;
        u16_at(d + 6, 2);
        f64_at(d + 24, offset);
        f64_at(d + 32, slope);
        return off;
    };
    auto add_cn = [&](uint8_t type, uint8_t sync, uint8_t dtype, uint8_t bit_off, uint32_t byte_off,
                      uint32_t nbits) {
        size_t off = add_block("##CN", 8, 12);
        size_t d = off + 24 + 64;
        b[d] = type;
        b[d + 1] = sync;
        b[d + 2] = dtype;
        b[d + 3] = bit_off;
        u32_at(d + 4, byte_off);
        u32_at(d + 8, nbits);
        return off;
    };

    b.assign(64, 0);
    std::memcpy(b.data(), "UnFinMF 4.11    ", 16);

    size_t hd = add_block("##HD", 6, 32);
    size_t hd_data = hd + 24 + 48;
    u64_at(hd_data, start_ns);
    b[hd_data + 12] = 0x02;

    size_t tx_acq = add_tx("CAN" + std::to_string(bus) + "_Rx");
    size_t tx_time = add_tx("Timestamp");
    size_t tx_root = add_tx("CAN_DataFrame");
    size_t tx_bus = add_tx("CAN_DataFrame.BusChannel");
    size_t tx_id = add_tx("CAN_DataFrame.ID");
    size_t tx_ide = add_tx("CAN_DataFrame.IDE");
    size_t tx_dlc = add_tx("CAN_DataFrame.DLC");
    size_t tx_len = add_tx("CAN_DataFrame.DataLength");
    size_t tx_bytes = add_tx("CAN_DataFrame.DataBytes");
    size_t tx_dir = add_tx("CAN_DataFrame.Dir");

    size_t cc_time = add_cc(0.0, 1e-6);
    size_t cc_bus = add_cc(static_cast<double>(bus), 0.0);
    size_t cc_ide = add_cc(0.0, 0.0);
    size_t cc_dir = add_cc(0.0, 0.0);

    size_t cn_time = add_cn(2, 1, 0, 0, 0, 48);
    size_t cn_root = add_cn(0, 0, 10, 0, 6, 80);
    size_t cn_bus = add_cn(6, 0, 0, 0, 0, 0);
    size_t cn_id = add_cn(0, 0, 0, 0, 6, 11);
    size_t cn_ide = add_cn(6, 0, 0, 0, 0, 0);
    size_t cn_dlc = add_cn(0, 0, 0, 4, 7, 4);
    size_t cn_len = add_cn(0, 0, 0, 4, 7, 4);
    size_t cn_bytes = add_cn(0, 0, 10, 0, 8, 64);
    size_t cn_dir = add_cn(6, 0, 0, 0, 0, 0);

    size_t cg = add_block("##CG", 6, 32);
    size_t cg_data = cg + 24 + 48;
    u64_at(cg_data, 1);
    u16_at(cg_data + 16, 0x0006);
    u32_at(cg_data + 24, 16);

    size_t dg = add_block("##DG", 4, 8);
    b[dg + 24 + 32] = 1;

    uint8_t rec[16] = {};
    for (int i = 0; i < 6; ++i) rec[i] = static_cast<uint8_t>(raw_us >> (8 * i));
    uint32_t id_bits = can_id & 0x7ffu;
    rec[6] = static_cast<uint8_t>(id_bits);
    rec[7] = static_cast<uint8_t>((id_bits >> 8) & 0x7u);
    rec[7] = static_cast<uint8_t>(rec[7] | ((dlc & 0x0fu) << 4));
    for (size_t i = 0; i < payload.size() && i < 8; ++i) rec[8 + i] = payload[i];

    size_t dt = add_block("##DT", 0, 0);
    b.push_back(1);
    b.insert(b.end(), rec, rec + sizeof rec);

    link_at(hd, 0, dg);
    link_at(dg, 1, cg);
    link_at(dg, 2, dt);
    link_at(cg, 1, cn_time);
    link_at(cg, 2, tx_acq);

    link_at(cn_time, 0, cn_root);
    link_at(cn_time, 2, tx_time);
    link_at(cn_time, 4, cc_time);

    link_at(cn_root, 1, cn_bus);
    link_at(cn_root, 2, tx_root);

    link_at(cn_bus, 0, cn_id);
    link_at(cn_bus, 2, tx_bus);
    link_at(cn_bus, 4, cc_bus);

    link_at(cn_id, 0, cn_ide);
    link_at(cn_id, 2, tx_id);

    link_at(cn_ide, 0, cn_dlc);
    link_at(cn_ide, 2, tx_ide);
    link_at(cn_ide, 4, cc_ide);

    link_at(cn_dlc, 0, cn_len);
    link_at(cn_dlc, 2, tx_dlc);

    link_at(cn_len, 0, cn_bytes);
    link_at(cn_len, 2, tx_len);

    link_at(cn_bytes, 0, cn_dir);
    link_at(cn_bytes, 2, tx_bytes);

    link_at(cn_dir, 2, tx_dir);
    link_at(cn_dir, 4, cc_dir);

    return b;
}
