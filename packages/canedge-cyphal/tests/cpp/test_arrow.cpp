#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include "arrow_out.h"
#include "decode.h"

TEST(Arrow, WriteFrameIpcRoundTrip) {
    Frame f;
    f.channel = "CAN1";
    f.timestamp_us = 42;
    f.id = 7;
    f.dlc = 1;
    f.ide = true;
    f.data = {9};

    auto out_result = arrow::io::BufferOutputStream::Create();
    ASSERT_TRUE(out_result.ok()) << out_result.status().ToString();
    auto out = *out_result;
    arrow::Status st = write_frame_ipc(std::vector<Frame>{f}, out);
    ASSERT_TRUE(st.ok()) << st.ToString();

    auto buf_result = out->Finish();
    ASSERT_TRUE(buf_result.ok()) << buf_result.status().ToString();
    auto buffer = *buf_result;

    auto file = std::make_shared<arrow::io::BufferReader>(buffer);
    auto reader_result = arrow::ipc::RecordBatchStreamReader::Open(file);
    ASSERT_TRUE(reader_result.ok()) << reader_result.status().ToString();
    auto reader = *reader_result;

    std::string schema_str = reader->schema()->ToString();
    EXPECT_NE(schema_str.find("timestamp[us, tz=UTC]"), std::string::npos);

    auto batch_result = reader->Next();
    ASSERT_TRUE(batch_result.ok()) << batch_result.status().ToString();
    auto batch = *batch_result;
    ASSERT_NE(batch, nullptr);
    ASSERT_EQ(batch->num_rows(), 1);

    auto id_arr = std::static_pointer_cast<arrow::UInt32Array>(batch->GetColumnByName("id"));
    ASSERT_NE(id_arr, nullptr);
    EXPECT_EQ(id_arr->Value(0), 7u);

    auto data_arr = std::static_pointer_cast<arrow::BinaryArray>(batch->GetColumnByName("data"));
    ASSERT_NE(data_arr, nullptr);
    auto view = data_arr->GetView(0);
    std::vector<uint8_t> got(view.begin(), view.end());
    EXPECT_EQ(got, std::vector<uint8_t>({9}));
}