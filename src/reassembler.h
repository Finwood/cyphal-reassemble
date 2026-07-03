#ifndef CYPHAL_REASSEMBLE_REASSEMBLER_H
#define CYPHAL_REASSEMBLE_REASSEMBLER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <tuple>

extern "C" {
#include "canard.h"
}
#include "transfer.h"

struct ReassemblerStats {
    uint64_t frames_in = 0;
    uint64_t transfers_out = 0;
    uint64_t errors = 0;   // invalid frame layout, skipped
    uint64_t oom = 0;      // canardRxAccept out-of-memory, skipped
};

class Reassembler {
public:
    using TransferSink = std::function<void(const Transfer&)>;

    explicit Reassembler(std::size_t extent = 65535U,
                         CanardMicrosecond transfer_id_timeout_us = CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC);
    ~Reassembler();

    Reassembler(const Reassembler&) = delete;
    Reassembler& operator=(const Reassembler&) = delete;

    // Feed one CAN frame; completed transfers are delivered to `sink`.
    void ingest(int64_t timestamp_us, uint32_t extended_can_id,
                const uint8_t* data, std::size_t size, uint8_t rti,
                const TransferSink& sink);

    const ReassemblerStats& stats() const { return stats_; }

private:
    CanardInstance& message_instance();
    CanardInstance& service_instance(uint8_t dest_node_id);
    void ensure_subscription(CanardInstance& ins, CanardTransferKind kind, CanardPortID port_id);

    std::size_t extent_;
    CanardMicrosecond tid_timeout_us_;
    ReassemblerStats stats_;

    std::unique_ptr<CanardInstance> message_;
    std::map<uint8_t, std::unique_ptr<CanardInstance>> services_;
    // Subscriptions must never move while in use; own them here, keyed by (instance, kind, port_id).
    std::map<std::tuple<CanardInstance*, int, uint16_t>, std::unique_ptr<CanardRxSubscription>> subscriptions_;
};

#endif
