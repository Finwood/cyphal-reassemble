#include "arrow_io.h"

#include <arrow/api.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>

namespace {
arrow::Status validate_field(const arrow::Schema& schema, const std::string& name,
                             arrow::Type::type expected) {
    auto field = schema.GetFieldByName(name);
    if (field == nullptr) {
        return arrow::Status::Invalid("input schema missing required column: ", name);
    }
    if (field->type()->id() != expected) {
        return arrow::Status::Invalid("input column ", name, " has unexpected type ",
                                      field->type()->ToString());
    }
    return arrow::Status::OK();
}
}  // namespace

arrow::Status read_frames(std::shared_ptr<arrow::io::InputStream> source,
                          const std::function<void(const InputFrame&)>& sink) {
    ARROW_ASSIGN_OR_RAISE(auto reader, arrow::ipc::RecordBatchStreamReader::Open(source));
    auto schema = reader->schema();

    ARROW_RETURN_NOT_OK(validate_field(*schema, "timestamp", arrow::Type::TIMESTAMP));
    ARROW_RETURN_NOT_OK(validate_field(*schema, "id", arrow::Type::UINT32));
    ARROW_RETURN_NOT_OK(validate_field(*schema, "data", arrow::Type::BINARY));
    const bool has_rti = schema->GetFieldByName("rti") != nullptr;
    if (has_rti) {
        ARROW_RETURN_NOT_OK(validate_field(*schema, "rti", arrow::Type::UINT8));
    }

    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        ARROW_RETURN_NOT_OK(reader->ReadNext(&batch));
        if (batch == nullptr) {
            break;  // end of stream
        }
        auto ts = std::static_pointer_cast<arrow::TimestampArray>(batch->GetColumnByName("timestamp"));
        auto id = std::static_pointer_cast<arrow::UInt32Array>(batch->GetColumnByName("id"));
        auto data = std::static_pointer_cast<arrow::BinaryArray>(batch->GetColumnByName("data"));
        std::shared_ptr<arrow::UInt8Array> rti;
        if (has_rti) {
            rti = std::static_pointer_cast<arrow::UInt8Array>(batch->GetColumnByName("rti"));
        }
        for (int64_t i = 0; i < batch->num_rows(); ++i) {
            if (data->IsNull(i) || id->IsNull(i) || ts->IsNull(i)) {
                continue;
            }
            InputFrame f;
            f.timestamp_us = ts->Value(i);
            f.extended_can_id = id->Value(i);
            auto view = data->GetView(i);
            f.data.assign(view.begin(), view.end());
            f.rti = (has_rti && !rti->IsNull(i)) ? rti->Value(i) : 0U;
            sink(f);
        }
    }
    return arrow::Status::OK();
}

struct TransferWriter::Builders {
    arrow::TimestampBuilder timestamp{arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"),
                                      arrow::default_memory_pool()};
    arrow::StringBuilder type;
    arrow::Int16Builder id;
    arrow::UInt8Builder source;
    arrow::UInt8Builder dest;
    arrow::UInt8Builder priority;
    arrow::UInt8Builder transfer_id;
    arrow::BinaryBuilder payload;
    arrow::Int32Builder length;
};

std::shared_ptr<arrow::Schema> TransferWriter::schema() {
    return arrow::schema({
        arrow::field("timestamp", arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"), false),
        arrow::field("type", arrow::utf8(), false),
        arrow::field("id", arrow::int16(), false),
        arrow::field("source", arrow::uint8(), true),
        arrow::field("dest", arrow::uint8(), true),
        arrow::field("priority", arrow::uint8(), true),
        arrow::field("transfer_id", arrow::uint8(), true),
        arrow::field("payload", arrow::binary(), true),
        arrow::field("length", arrow::int32(), true),
    });
}

TransferWriter::TransferWriter(std::shared_ptr<arrow::io::OutputStream> sink, int64_t batch_size)
    : sink_(std::move(sink)), batch_size_(batch_size), b_(std::make_unique<Builders>()) {}

TransferWriter::~TransferWriter() = default;

arrow::Status TransferWriter::Append(const Transfer& t) {
    if (writer_ == nullptr) {
        ARROW_ASSIGN_OR_RAISE(writer_, arrow::ipc::MakeStreamWriter(sink_, schema()));
    }
    ARROW_RETURN_NOT_OK(b_->timestamp.Append(t.timestamp_us));
    ARROW_RETURN_NOT_OK(b_->type.Append(t.type));
    ARROW_RETURN_NOT_OK(b_->id.Append(t.id));
    if (t.source.has_value()) {
        ARROW_RETURN_NOT_OK(b_->source.Append(t.source.value()));
    } else {
        ARROW_RETURN_NOT_OK(b_->source.AppendNull());
    }
    if (t.dest.has_value()) {
        ARROW_RETURN_NOT_OK(b_->dest.Append(t.dest.value()));
    } else {
        ARROW_RETURN_NOT_OK(b_->dest.AppendNull());
    }
    ARROW_RETURN_NOT_OK(b_->priority.Append(t.priority));
    ARROW_RETURN_NOT_OK(b_->transfer_id.Append(t.transfer_id));
    ARROW_RETURN_NOT_OK(b_->payload.Append(t.payload.data(), static_cast<int32_t>(t.payload.size())));
    ARROW_RETURN_NOT_OK(b_->length.Append(t.length));
    rows_++;
    if (rows_ >= batch_size_) {
        ARROW_RETURN_NOT_OK(Flush());
    }
    return arrow::Status::OK();
}

arrow::Status TransferWriter::Flush() {
    if (rows_ == 0) {
        return arrow::Status::OK();
    }
    std::shared_ptr<arrow::Array> a_ts, a_type, a_id, a_src, a_dst, a_prio, a_tid, a_pl, a_len;
    ARROW_RETURN_NOT_OK(b_->timestamp.Finish(&a_ts));
    ARROW_RETURN_NOT_OK(b_->type.Finish(&a_type));
    ARROW_RETURN_NOT_OK(b_->id.Finish(&a_id));
    ARROW_RETURN_NOT_OK(b_->source.Finish(&a_src));
    ARROW_RETURN_NOT_OK(b_->dest.Finish(&a_dst));
    ARROW_RETURN_NOT_OK(b_->priority.Finish(&a_prio));
    ARROW_RETURN_NOT_OK(b_->transfer_id.Finish(&a_tid));
    ARROW_RETURN_NOT_OK(b_->payload.Finish(&a_pl));
    ARROW_RETURN_NOT_OK(b_->length.Finish(&a_len));
    auto batch = arrow::RecordBatch::Make(schema(), rows_,
        {a_ts, a_type, a_id, a_src, a_dst, a_prio, a_tid, a_pl, a_len});
    ARROW_RETURN_NOT_OK(writer_->WriteRecordBatch(*batch));
    rows_ = 0;
    return arrow::Status::OK();
}

arrow::Status TransferWriter::Close() {
    // Ensure a writer exists even for empty output, so a valid (schema-only) stream is emitted.
    if (writer_ == nullptr) {
        ARROW_ASSIGN_OR_RAISE(writer_, arrow::ipc::MakeStreamWriter(sink_, schema()));
    }
    ARROW_RETURN_NOT_OK(Flush());
    ARROW_RETURN_NOT_OK(writer_->Close());
    return arrow::Status::OK();
}
