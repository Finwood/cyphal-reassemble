#include "arrow_out.h"

#include <memory>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/ipc/api.h>

namespace {

std::shared_ptr<arrow::Schema> frame_schema() {
    return arrow::schema({
        arrow::field("channel", arrow::utf8()),
        arrow::field("timestamp", arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"), false),
        arrow::field("id", arrow::uint32()),
        arrow::field("dlc", arrow::uint8()),
        arrow::field("ide", arrow::boolean()),
        arrow::field("rtr", arrow::boolean()),
        arrow::field("err", arrow::boolean()),
        arrow::field("edl", arrow::boolean()),
        arrow::field("brs", arrow::boolean()),
        arrow::field("esi", arrow::boolean()),
        arrow::field("tx", arrow::boolean()),
        arrow::field("data", arrow::binary()),
    });
}

}  // namespace

arrow::Status write_frame_ipc(const std::vector<Frame>& frames,
                              const std::shared_ptr<arrow::io::OutputStream>& sink) {
    auto schema = frame_schema();
    const int64_t n = static_cast<int64_t>(frames.size());

    arrow::StringBuilder channel_b;
    arrow::TimestampBuilder timestamp_b{arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"),
                                         arrow::default_memory_pool()};
    arrow::UInt32Builder id_b;
    arrow::UInt8Builder dlc_b;
    arrow::BooleanBuilder ide_b;
    arrow::BooleanBuilder rtr_b;
    arrow::BooleanBuilder err_b;
    arrow::BooleanBuilder edl_b;
    arrow::BooleanBuilder brs_b;
    arrow::BooleanBuilder esi_b;
    arrow::BooleanBuilder tx_b;
    arrow::BinaryBuilder data_b;

    RETURN_NOT_OK(channel_b.Reserve(n));
    RETURN_NOT_OK(timestamp_b.Reserve(n));
    RETURN_NOT_OK(id_b.Reserve(n));
    RETURN_NOT_OK(dlc_b.Reserve(n));
    RETURN_NOT_OK(ide_b.Reserve(n));
    RETURN_NOT_OK(rtr_b.Reserve(n));
    RETURN_NOT_OK(err_b.Reserve(n));
    RETURN_NOT_OK(edl_b.Reserve(n));
    RETURN_NOT_OK(brs_b.Reserve(n));
    RETURN_NOT_OK(esi_b.Reserve(n));
    RETURN_NOT_OK(tx_b.Reserve(n));
    RETURN_NOT_OK(data_b.Reserve(n));

    int64_t channel_bytes = 0;
    int64_t payload_bytes = 0;
    for (const auto& f : frames) {
        channel_bytes += static_cast<int64_t>(f.channel.size());
        payload_bytes += static_cast<int64_t>(f.data.size());
    }
    RETURN_NOT_OK(channel_b.ReserveData(channel_bytes));
    RETURN_NOT_OK(data_b.ReserveData(payload_bytes));

    std::vector<int64_t> timestamps(static_cast<size_t>(n));
    std::vector<uint32_t> ids(static_cast<size_t>(n));
    std::vector<uint8_t> dlcs(static_cast<size_t>(n));
    std::vector<uint8_t> ide(static_cast<size_t>(n));
    std::vector<uint8_t> rtr(static_cast<size_t>(n));
    std::vector<uint8_t> err(static_cast<size_t>(n));
    std::vector<uint8_t> edl(static_cast<size_t>(n));
    std::vector<uint8_t> brs(static_cast<size_t>(n));
    std::vector<uint8_t> esi(static_cast<size_t>(n));
    std::vector<uint8_t> tx(static_cast<size_t>(n));
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& f = frames[i];
        timestamps[i] = f.timestamp_us;
        ids[i] = f.id;
        dlcs[i] = f.dlc;
        ide[i] = static_cast<uint8_t>(f.ide);
        rtr[i] = static_cast<uint8_t>(f.rtr);
        err[i] = static_cast<uint8_t>(f.err);
        edl[i] = static_cast<uint8_t>(f.edl);
        brs[i] = static_cast<uint8_t>(f.brs);
        esi[i] = static_cast<uint8_t>(f.esi);
        tx[i] = static_cast<uint8_t>(f.tx);
        RETURN_NOT_OK(channel_b.Append(f.channel.data(), static_cast<int32_t>(f.channel.size())));
        RETURN_NOT_OK(data_b.Append(f.data.data(), static_cast<int32_t>(f.data.size())));
    }
    if (n > 0) {
        RETURN_NOT_OK(timestamp_b.AppendValues(timestamps.data(), n));
        RETURN_NOT_OK(id_b.AppendValues(ids.data(), n));
        RETURN_NOT_OK(dlc_b.AppendValues(dlcs.data(), n));
        RETURN_NOT_OK(ide_b.AppendValues(ide.data(), n));
        RETURN_NOT_OK(rtr_b.AppendValues(rtr.data(), n));
        RETURN_NOT_OK(err_b.AppendValues(err.data(), n));
        RETURN_NOT_OK(edl_b.AppendValues(edl.data(), n));
        RETURN_NOT_OK(brs_b.AppendValues(brs.data(), n));
        RETURN_NOT_OK(esi_b.AppendValues(esi.data(), n));
        RETURN_NOT_OK(tx_b.AppendValues(tx.data(), n));
    }

    ARROW_ASSIGN_OR_RAISE(auto channel_arr, channel_b.Finish());
    ARROW_ASSIGN_OR_RAISE(auto timestamp_arr, timestamp_b.Finish());
    ARROW_ASSIGN_OR_RAISE(auto id_arr, id_b.Finish());
    ARROW_ASSIGN_OR_RAISE(auto dlc_arr, dlc_b.Finish());
    ARROW_ASSIGN_OR_RAISE(auto ide_arr, ide_b.Finish());
    ARROW_ASSIGN_OR_RAISE(auto rtr_arr, rtr_b.Finish());
    ARROW_ASSIGN_OR_RAISE(auto err_arr, err_b.Finish());
    ARROW_ASSIGN_OR_RAISE(auto edl_arr, edl_b.Finish());
    ARROW_ASSIGN_OR_RAISE(auto brs_arr, brs_b.Finish());
    ARROW_ASSIGN_OR_RAISE(auto esi_arr, esi_b.Finish());
    ARROW_ASSIGN_OR_RAISE(auto tx_arr, tx_b.Finish());
    ARROW_ASSIGN_OR_RAISE(auto data_arr, data_b.Finish());

    auto batch = arrow::RecordBatch::Make(
        schema, n,
        {channel_arr, timestamp_arr, id_arr, dlc_arr, ide_arr, rtr_arr, err_arr, edl_arr, brs_arr,
         esi_arr, tx_arr, data_arr});

    ARROW_ASSIGN_OR_RAISE(auto writer, arrow::ipc::MakeStreamWriter(sink, schema));
    RETURN_NOT_OK(writer->WriteRecordBatch(*batch));
    RETURN_NOT_OK(writer->Close());
    return arrow::Status::OK();
}