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

    arrow::StringBuilder channel_b;
    arrow::Int64Builder timestamp_b;
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

    for (const auto& f : frames) {
        RETURN_NOT_OK(channel_b.Append(f.channel));
        RETURN_NOT_OK(timestamp_b.Append(f.timestamp_us));
        RETURN_NOT_OK(id_b.Append(f.id));
        RETURN_NOT_OK(dlc_b.Append(f.dlc));
        RETURN_NOT_OK(ide_b.Append(f.ide));
        RETURN_NOT_OK(rtr_b.Append(f.rtr));
        RETURN_NOT_OK(err_b.Append(f.err));
        RETURN_NOT_OK(edl_b.Append(f.edl));
        RETURN_NOT_OK(brs_b.Append(f.brs));
        RETURN_NOT_OK(esi_b.Append(f.esi));
        RETURN_NOT_OK(tx_b.Append(f.tx));
        RETURN_NOT_OK(data_b.Append(f.data.data(), static_cast<int32_t>(f.data.size())));
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

    const int64_t n = static_cast<int64_t>(frames.size());
    auto batch = arrow::RecordBatch::Make(
        schema, n,
        {channel_arr, timestamp_arr, id_arr, dlc_arr, ide_arr, rtr_arr, err_arr, edl_arr, brs_arr,
         esi_arr, tx_arr, data_arr});

    ARROW_ASSIGN_OR_RAISE(auto writer, arrow::ipc::MakeStreamWriter(sink, schema));
    RETURN_NOT_OK(writer->WriteRecordBatch(*batch));
    RETURN_NOT_OK(writer->Close());
    return arrow::Status::OK();
}