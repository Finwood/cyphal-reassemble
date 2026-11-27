#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// Unfinalized MF4 buffers for decode tests.
//
// Classic group: 16-byte record, 48-bit timestamp at byte 0, 11-bit ID at
// byte 6 bit 0, 4-bit DLC and DataLength at byte 7 bit 4, 8 inline data
// bytes at byte 8. BusChannel, IDE, and Dir are virtual. Timestamp slope
// defaults to the float64 of 1e-6.
//
// Error group: 7-byte record, same 48-bit timestamp, ErrorType is 6 bits at
// byte 6 bit 0, BusChannel virtual.
//
// CAN FD group: 13-byte record. DLC and DataLength are the same 4 bits at
// byte 8 bit 0. BRS is byte 8 bit 6. ESI is byte 8 bit 7. DataBytes is
// cn_type 1, 32 bits at byte 9, and cn_data points at the following VLSD
// channel group. EDL is virtual 1. The VLSD group has cg_flags bit 0 set
// and cg_data_bytes 0. Each VLSD record is a little-endian uint32 length
// plus that many payload bytes.

class MdfBuilder {
 public:
    uint64_t start_ns = 0;
    uint8_t time_flags = 0x02;

    void add_classic_group(uint8_t record_id, uint32_t bus, double time_slope = 1e-6) {
        groups_.push_back(Group{Kind::Classic, record_id, bus, time_slope, 0});
    }

    void add_error_group(uint8_t record_id, uint32_t bus, double time_slope = 1e-6) {
        groups_.push_back(Group{Kind::Error, record_id, bus, time_slope, 0});
    }

    void add_remote_group(uint8_t record_id) {
        groups_.push_back(Group{Kind::Remote, record_id, 0, 1e-6, 0});
    }

    void add_fd_group(uint8_t record_id, uint8_t vlsd_record_id, uint32_t bus, double time_slope = 1e-6) {
        groups_.push_back(Group{Kind::Fd, record_id, bus, time_slope, vlsd_record_id});
        groups_.push_back(Group{Kind::Vlsd, vlsd_record_id, 0, 1e-6, 0});
    }

    void add_classic_record(uint8_t record_id, uint64_t raw_us, uint32_t can_id, uint8_t dlc,
                            std::vector<uint8_t> payload) {
        records_.push_back(record_id);
        append_u48(records_, raw_us);
        uint32_t id_bits = can_id & 0x7ffu;
        records_.push_back(static_cast<uint8_t>(id_bits));
        uint8_t flags = static_cast<uint8_t>(((id_bits >> 8) & 0x7u) | ((dlc & 0x0fu) << 4));
        records_.push_back(flags);
        for (size_t i = 0; i < 8; ++i) {
            records_.push_back(i < payload.size() ? payload[i] : 0);
        }
    }

    void add_error_record(uint8_t record_id, uint64_t raw_us, uint8_t error_type) {
        records_.push_back(record_id);
        append_u48(records_, raw_us);
        records_.push_back(static_cast<uint8_t>(error_type & 0x3fu));
    }

    void add_remote_record(uint8_t record_id) {
        records_.push_back(record_id);
        records_.insert(records_.end(), 8, 0);
    }

    void add_fd_record(uint8_t record_id, uint64_t raw_us, uint32_t can_id, uint8_t dlc, uint32_t vlsd_offset,
                       bool brs, bool esi) {
        records_.push_back(record_id);
        append_u48(records_, raw_us);
        uint32_t id_bits = can_id & 0x7ffu;
        records_.push_back(static_cast<uint8_t>(id_bits));
        records_.push_back(static_cast<uint8_t>((id_bits >> 8) & 0x7u));
        uint8_t flags = static_cast<uint8_t>(dlc & 0x0fu);
        if (brs) flags = static_cast<uint8_t>(flags | 0x40u);
        if (esi) flags = static_cast<uint8_t>(flags | 0x80u);
        records_.push_back(flags);
        for (int i = 0; i < 4; ++i) records_.push_back(static_cast<uint8_t>(vlsd_offset >> (8 * i)));
    }

    void add_vlsd_record(uint8_t record_id, std::vector<uint8_t> payload) {
        records_.push_back(record_id);
        uint32_t n = static_cast<uint32_t>(payload.size());
        for (int i = 0; i < 4; ++i) records_.push_back(static_cast<uint8_t>(n >> (8 * i)));
        records_.insert(records_.end(), payload.begin(), payload.end());
    }

    void set_dt_payload(std::vector<uint8_t> bytes) {
        use_raw_dt_ = true;
        raw_dt_ = std::move(bytes);
    }

    std::vector<uint8_t> build() const {
        std::vector<uint8_t> b(64, 0);
        std::memcpy(b.data(), "UnFinMF 4.11    ", 16);

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
        auto add_cg = [&](uint8_t record_id, uint16_t flags, uint32_t data_bytes) {
            size_t off = add_block("##CG", 6, 32);
            size_t d = off + 24 + 48;
            u64_at(d, record_id);
            u16_at(d + 16, flags);
            u32_at(d + 24, data_bytes);
            return off;
        };

        size_t hd = add_block("##HD", 6, 32);
        size_t hd_data = hd + 24 + 48;
        u64_at(hd_data, start_ns);
        b[hd_data + 12] = time_flags;

        struct Built {
            Kind kind = Kind::Classic;
            uint8_t record_id = 0;
            uint8_t vlsd_record_id = 0;
            size_t cg = 0;
            size_t bytes_cn = 0;
        };
        std::vector<Built> built;
        built.reserve(groups_.size());

        for (const Group& g : groups_) {
            Built out;
            out.kind = g.kind;
            out.record_id = g.record_id;
            out.vlsd_record_id = g.vlsd_record_id;

            if (g.kind == Kind::Vlsd) {
                out.cg = add_cg(g.record_id, 0x0001, 0);
                built.push_back(out);
                continue;
            }
            if (g.kind == Kind::Remote) {
                size_t tx_root = add_tx("CAN_RemoteFrame");
                size_t cn_root = add_cn(0, 0, 0, 0, 0, 0);
                link_at(cn_root, 2, tx_root);
                out.cg = add_cg(g.record_id, 0x0006, 8);
                link_at(out.cg, 1, cn_root);
                size_t tx_acq = add_tx("CAN_Remote");
                link_at(out.cg, 2, tx_acq);
                built.push_back(out);
                continue;
            }

            const bool fd = g.kind == Kind::Fd;
            const bool error = g.kind == Kind::Error;
            const char* root_name = error ? "CAN_ErrorFrame" : "CAN_DataFrame";
            std::string acq = std::string("CAN") + std::to_string(g.bus) + (error ? "_Error" : "_Rx");

            size_t tx_acq = add_tx(acq);
            size_t tx_time = add_tx("Timestamp");
            size_t tx_root = add_tx(root_name);
            size_t tx_bus = add_tx(std::string(root_name) + ".BusChannel");
            size_t cc_time = add_cc(0.0, g.time_slope);
            size_t cc_bus = add_cc(static_cast<double>(g.bus), 0.0);
            size_t cn_time = add_cn(2, 1, 0, 0, 0, 48);
            size_t cn_bus = add_cn(6, 0, 0, 0, 0, 0);

            link_at(cn_time, 2, tx_time);
            link_at(cn_time, 4, cc_time);
            link_at(cn_bus, 2, tx_bus);
            link_at(cn_bus, 4, cc_bus);

            size_t cn_root = 0;
            if (error) {
                size_t tx_type = add_tx("CAN_ErrorFrame.ErrorType");
                size_t cn_type = add_cn(0, 0, 0, 0, 6, 6);
                link_at(cn_type, 2, tx_type);
                cn_root = add_cn(0, 0, 10, 0, 6, 8);
                link_at(cn_root, 1, cn_bus);
                link_at(cn_root, 2, tx_root);
                link_at(cn_bus, 0, cn_type);
            } else {
                size_t tx_id = add_tx(std::string(root_name) + ".ID");
                size_t tx_ide = add_tx(std::string(root_name) + ".IDE");
                size_t tx_dlc = add_tx(std::string(root_name) + ".DLC");
                size_t tx_len = add_tx(std::string(root_name) + ".DataLength");
                size_t tx_bytes = add_tx(std::string(root_name) + ".DataBytes");
                size_t tx_dir = add_tx(std::string(root_name) + ".Dir");
                size_t cc_ide = add_cc(0.0, 0.0);
                size_t cc_dir = add_cc(0.0, 0.0);

                size_t cn_id = add_cn(0, 0, 0, 0, 6, 11);
                size_t cn_ide = add_cn(6, 0, 0, 0, 0, 0);
                // Classic DLC/DataLength share byte 7 bit 4. CAN FD shares byte 8 bit 0.
                size_t cn_dlc = fd ? add_cn(0, 0, 0, 0, 8, 4) : add_cn(0, 0, 0, 4, 7, 4);
                size_t cn_len = fd ? add_cn(0, 0, 0, 0, 8, 4) : add_cn(0, 0, 0, 4, 7, 4);
                size_t cn_bytes = fd ? add_cn(1, 0, 10, 0, 9, 32) : add_cn(0, 0, 10, 0, 8, 64);
                size_t cn_dir = add_cn(6, 0, 0, 0, 0, 0);
                cn_root = fd ? add_cn(0, 0, 10, 0, 6, 56) : add_cn(0, 0, 10, 0, 6, 80);

                link_at(cn_root, 1, cn_bus);
                link_at(cn_root, 2, tx_root);
                link_at(cn_bus, 0, cn_id);
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
                out.bytes_cn = cn_bytes;

                if (fd) {
                    size_t tx_edl = add_tx("CAN_DataFrame.EDL");
                    size_t tx_brs = add_tx("CAN_DataFrame.BRS");
                    size_t tx_esi = add_tx("CAN_DataFrame.ESI");
                    size_t cc_edl = add_cc(1.0, 0.0);
                    size_t cn_edl = add_cn(6, 0, 0, 0, 0, 0);
                    size_t cn_brs = add_cn(0, 0, 0, 6, 8, 1);
                    size_t cn_esi = add_cn(0, 0, 0, 7, 8, 1);
                    link_at(cn_dir, 0, cn_edl);
                    link_at(cn_edl, 0, cn_brs);
                    link_at(cn_edl, 2, tx_edl);
                    link_at(cn_edl, 4, cc_edl);
                    link_at(cn_brs, 0, cn_esi);
                    link_at(cn_brs, 2, tx_brs);
                    link_at(cn_esi, 2, tx_esi);
                }
            }

            link_at(cn_time, 0, cn_root);
            uint32_t data_bytes = error ? 7u : (fd ? 13u : 16u);
            out.cg = add_cg(g.record_id, 0x0006, data_bytes);
            link_at(out.cg, 1, cn_time);
            link_at(out.cg, 2, tx_acq);
            built.push_back(out);
        }

        for (size_t i = 0; i + 1 < built.size(); ++i) link_at(built[i].cg, 0, built[i + 1].cg);
        for (const Built& group : built) {
            if (group.kind != Kind::Fd) continue;
            for (const Built& other : built) {
                if (other.kind == Kind::Vlsd && other.record_id == group.vlsd_record_id) {
                    link_at(group.bytes_cn, 5, other.cg);
                    break;
                }
            }
        }

        size_t dg = add_block("##DG", 4, 8);
        b[dg + 24 + 32] = 1;
        size_t dt = add_block("##DT", 0, 0);
        if (use_raw_dt_) {
            b.insert(b.end(), raw_dt_.begin(), raw_dt_.end());
        } else {
            b.insert(b.end(), records_.begin(), records_.end());
        }

        link_at(hd, 0, dg);
        if (!built.empty()) link_at(dg, 1, built.front().cg);
        link_at(dg, 2, dt);
        return b;
    }

 private:
    enum class Kind { Classic, Error, Remote, Fd, Vlsd };

    struct Group {
        Kind kind;
        uint8_t record_id;
        uint32_t bus;
        double time_slope;
        uint8_t vlsd_record_id;
    };

    static void append_u48(std::vector<uint8_t>& out, uint64_t raw) {
        for (int i = 0; i < 6; ++i) out.push_back(static_cast<uint8_t>(raw >> (8 * i)));
    }

    std::vector<Group> groups_;
    std::vector<uint8_t> records_;
    bool use_raw_dt_ = false;
    std::vector<uint8_t> raw_dt_;
};

inline std::vector<uint8_t> build_classic_rx(uint64_t start_ns, uint64_t raw_us, uint32_t bus, uint32_t can_id,
                                             uint8_t dlc, std::vector<uint8_t> payload) {
    MdfBuilder builder;
    builder.start_ns = start_ns;
    builder.add_classic_group(1, bus);
    builder.add_classic_record(1, raw_us, can_id, dlc, std::move(payload));
    return builder.build();
}
