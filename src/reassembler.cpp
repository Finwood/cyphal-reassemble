#include "reassembler.h"

#include <cstdlib>

extern "C" {
#include "can_id.h"
}

namespace {
void* mem_allocate(void* /*user*/, std::size_t size) { return std::malloc(size); }
void mem_deallocate(void* /*user*/, std::size_t /*size*/, void* ptr) { std::free(ptr); }

CanardMemoryResource make_memory() {
    CanardMemoryResource m;
    m.user_reference = nullptr;
    m.deallocate = &mem_deallocate;
    m.allocate = &mem_allocate;
    return m;
}
}  // namespace

Reassembler::Reassembler(std::size_t extent, CanardMicrosecond transfer_id_timeout_us)
    : extent_(extent), tid_timeout_us_(transfer_id_timeout_us) {}

Reassembler::~Reassembler() = default;

CanardInstance& Reassembler::message_instance() {
    if (!message_) {
        message_ = std::make_unique<CanardInstance>(canardInit(make_memory()));
        message_->node_id = CANARD_NODE_ID_UNSET;  // anonymous; accepts all broadcast messages
    }
    return *message_;
}

CanardInstance& Reassembler::service_instance(uint8_t dest_node_id) {
    auto it = services_.find(dest_node_id);
    if (it == services_.end()) {
        auto ins = std::make_unique<CanardInstance>(canardInit(make_memory()));
        ins->node_id = dest_node_id;  // so canardRxAccept accepts services addressed to this dest
        it = services_.emplace(dest_node_id, std::move(ins)).first;
    }
    return *it->second;
}

void Reassembler::ensure_subscription(CanardInstance& ins, CanardTransferKind kind, CanardPortID port_id) {
    auto key = std::make_tuple(&ins, static_cast<int>(kind), static_cast<uint16_t>(port_id));
    if (subscriptions_.count(key) != 0) {
        return;
    }
    auto sub = std::make_unique<CanardRxSubscription>();
    (void)canardRxSubscribe(&ins, kind, port_id, extent_, tid_timeout_us_, sub.get());
    subscriptions_.emplace(std::move(key), std::move(sub));
}

void Reassembler::ingest(int64_t timestamp_us, uint32_t extended_can_id,
                         const uint8_t* data, std::size_t size, uint8_t rti,
                         const TransferSink& sink) {
    stats_.frames_in++;

    const cy_can_id_t decoded = cy_decode_can_id(extended_can_id);
    if (!decoded.valid) {
        stats_.errors++;
        return;
    }

    CanardInstance* ins = nullptr;
    CanardTransferKind kind = CanardTransferKindMessage;
    if (decoded.kind == CY_KIND_MESSAGE) {
        ins = &message_instance();
        kind = CanardTransferKindMessage;
    } else {
        ins = &service_instance(decoded.dest_node_id);
        kind = (decoded.kind == CY_KIND_REQUEST) ? CanardTransferKindRequest : CanardTransferKindResponse;
    }

    ensure_subscription(*ins, kind, decoded.port_id);

    CanardFrame frame;
    frame.extended_can_id = extended_can_id;
    frame.payload.size = size;
    frame.payload.data = data;

    CanardRxTransfer transfer;
    CanardRxSubscription* sub = nullptr;
    const int8_t result = canardRxAccept(ins, static_cast<CanardMicrosecond>(timestamp_us),
                                         &frame, rti, &transfer, &sub);
    if (result < 0) {
        stats_.oom++;
        return;
    }
    if (result != 1) {
        return;  // frame accepted but transfer not yet complete
    }

    Transfer out;
    out.timestamp_us = static_cast<int64_t>(transfer.timestamp_usec);
    switch (transfer.metadata.transfer_kind) {
        case CanardTransferKindMessage: out.type = "Message"; break;
        case CanardTransferKindRequest: out.type = "Request"; break;
        case CanardTransferKindResponse: out.type = "Response"; break;
        default: out.type = "Message"; break;
    }
    out.id = static_cast<int16_t>(transfer.metadata.port_id);
    if (transfer.metadata.remote_node_id == CANARD_NODE_ID_UNSET) {
        out.source = std::nullopt;
    } else {
        out.source = static_cast<uint8_t>(transfer.metadata.remote_node_id);
    }
    if (transfer.metadata.transfer_kind == CanardTransferKindMessage) {
        out.dest = std::nullopt;
    } else {
        out.dest = static_cast<uint8_t>(ins->node_id);
    }
    out.priority = static_cast<uint8_t>(transfer.metadata.priority);
    out.transfer_id = static_cast<uint8_t>(transfer.metadata.transfer_id);
    const auto* bytes = static_cast<const uint8_t*>(transfer.payload.data);
    out.payload.assign(bytes, bytes + transfer.payload.size);
    out.length = static_cast<int32_t>(transfer.payload.size);

    // Release the payload buffer allocated by libcanard.
    ins->memory.deallocate(ins->memory.user_reference, transfer.payload.allocated_size,
                           transfer.payload.data);

    stats_.transfers_out++;
    sink(out);
}
