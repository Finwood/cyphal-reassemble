#include <gtest/gtest.h>

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>

#include <memory>
#include <vector>

#include "arrow_io.h"

namespace {
// Serialize a RecordBatch to an in-memory Arrow IPC stream buffer.
std::shared_ptr<arrow::Buffer> to_ipc_stream(const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    auto writer = arrow::ipc::MakeStreamWriter(sink, batch->schema()).ValueOrDie();
    (void)writer->WriteRecordBatch(*batch);
    (void)writer->Close();
    return sink->Finish().ValueOrDie();
}

std::shared_ptr<arrow::RecordBatch> make_input_batch() {
    auto ts_type = arrow::timestamp(arrow::TimeUnit::MICRO, "UTC");
    arrow::TimestampBuilder ts_b(ts_type, arrow::default_memory_pool());
    arrow::UInt32Builder id_b;
    arrow::BinaryBuilder data_b;
    // One heartbeat frame: 7 data bytes + tail 0xF5.
    (void)ts_b.Append(1000);
    (void)id_b.Append(0x101D552AU);
    const uint8_t payload[] = {0x55, 0x02, 0x00, 0x00, 0x00, 0x00, 0x09, 0xF5};
    (void)data_b.Append(payload, sizeof(payload));
    std::shared_ptr<arrow::Array> ts_a, id_a, data_a;
    (void)ts_b.Finish(&ts_a);
    (void)id_b.Finish(&id_a);
    (void)data_b.Finish(&data_a);
    auto schema = arrow::schema({
        arrow::field("timestamp", ts_type, false),
        arrow::field("id", arrow::uint32()),
        arrow::field("data", arrow::binary()),
    });
    return arrow::RecordBatch::Make(schema, 1, {ts_a, id_a, data_a});
}
}  // namespace

TEST(ArrowInput, ReadsFramesFromStream) {
    auto buf = to_ipc_stream(make_input_batch());
    auto source = std::make_shared<arrow::io::BufferReader>(buf);

    std::vector<InputFrame> frames;
    auto status = read_frames(source, [&](const InputFrame& f) { frames.push_back(f); });
    ASSERT_TRUE(status.ok()) << status.ToString();
    ASSERT_EQ(frames.size(), 1U);
    EXPECT_EQ(frames[0].timestamp_us, 1000);
    EXPECT_EQ(frames[0].extended_can_id, 0x101D552AU);
    EXPECT_EQ(frames[0].data.size(), 8U);
    EXPECT_EQ(frames[0].rti, 0U);
}

TEST(ArrowInput, RejectsMissingRequiredColumn) {
    // Batch missing the `data` column.
    auto ts_type = arrow::timestamp(arrow::TimeUnit::MICRO, "UTC");
    arrow::TimestampBuilder ts_b(ts_type, arrow::default_memory_pool());
    arrow::UInt32Builder id_b;
    (void)ts_b.Append(1);
    (void)id_b.Append(1);
    std::shared_ptr<arrow::Array> ts_a, id_a;
    (void)ts_b.Finish(&ts_a);
    (void)id_b.Finish(&id_a);
    auto schema = arrow::schema({
        arrow::field("timestamp", ts_type, false),
        arrow::field("id", arrow::uint32()),
    });
    auto batch = arrow::RecordBatch::Make(schema, 1, {ts_a, id_a});
    auto buf = to_ipc_stream(batch);
    auto source = std::make_shared<arrow::io::BufferReader>(buf);

    std::vector<InputFrame> frames;
    auto status = read_frames(source, [&](const InputFrame& f) { frames.push_back(f); });
    EXPECT_FALSE(status.ok());
}

TEST(ArrowOutput, WritesTransfersRoundTrip) {
    auto sink = arrow::io::BufferOutputStream::Create().ValueOrDie();
    {
        TransferWriter writer(sink);
        Transfer msg;
        msg.timestamp_us = 1000;
        msg.type = "Message";
        msg.id = 7509;
        msg.source = 42;
        msg.dest = std::nullopt;
        msg.priority = 4;
        msg.transfer_id = 21;
        msg.payload = {0x55, 0x02};
        msg.length = 2;
        ASSERT_TRUE(writer.Append(msg).ok());

        Transfer resp;
        resp.timestamp_us = 2000;
        resp.type = "Response";
        resp.id = 430;
        resp.source = 125;
        resp.dest = 42;
        resp.priority = 4;
        resp.transfer_id = 3;
        resp.payload = {0xAA};
        resp.length = 1;
        ASSERT_TRUE(writer.Append(resp).ok());
        ASSERT_TRUE(writer.Close().ok());
    }

    auto buf = sink->Finish().ValueOrDie();
    auto source = std::make_shared<arrow::io::BufferReader>(buf);
    auto reader = arrow::ipc::RecordBatchStreamReader::Open(source).ValueOrDie();

    // Schema matches TransferSchema minus `channel`.
    auto schema = reader->schema();
    ASSERT_EQ(schema->num_fields(), 9);
    EXPECT_EQ(schema->field(0)->name(), "timestamp");
    EXPECT_EQ(schema->field(1)->name(), "type");
    EXPECT_EQ(schema->field(2)->name(), "id");
    EXPECT_EQ(schema->field(2)->type()->id(), arrow::Type::INT16);
    EXPECT_EQ(schema->field(3)->name(), "source");
    EXPECT_EQ(schema->field(3)->type()->id(), arrow::Type::UINT8);
    EXPECT_EQ(schema->field(9 - 1)->name(), "length");

    auto table_result = reader->ToTable();
    ASSERT_TRUE(table_result.ok());
    std::shared_ptr<arrow::Table> table = table_result.ValueOrDie();
    ASSERT_EQ(table->num_rows(), 2);

    auto type_col = std::static_pointer_cast<arrow::StringArray>(table->column(1)->chunk(0));
    auto dest_col = std::static_pointer_cast<arrow::UInt8Array>(table->column(3 + 1)->chunk(0));
    EXPECT_EQ(type_col->GetString(0), "Message");
    EXPECT_TRUE(dest_col->IsNull(0));      // message has no destination
    EXPECT_FALSE(dest_col->IsNull(1));     // response has a destination
    EXPECT_EQ(dest_col->Value(1), 42);
}
