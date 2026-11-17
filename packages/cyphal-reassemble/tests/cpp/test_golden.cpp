#include <gtest/gtest.h>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/ipc/reader.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "arrow_io.h"
#include "reassembler.h"

namespace {
// Fixtures directory is provided at compile time via FIXTURE_DIR.
std::string fixture(const std::string& name) {
    return std::string(FIXTURE_DIR) + "/" + name;
}

// Identity key for tolerant comparison: ignore timestamp (documented to differ).
using Key = std::tuple<std::string, int, int, int, int, std::vector<uint8_t>>;

Key key_of(const std::string& type, int id, int source, int dest, int transfer_id,
           const std::vector<uint8_t>& payload) {
    return std::make_tuple(type, id, source, dest, transfer_id, payload);
}

std::multiset<Key> reassemble_fixture(const std::string& frames_file) {
    auto infile = arrow::io::ReadableFile::Open(frames_file).ValueOrDie();
    Reassembler r;
    std::multiset<Key> out;
    auto status = read_frames(infile, [&](const InputFrame& f) {
        r.ingest(f.timestamp_us, f.extended_can_id, f.data.data(), f.data.size(), f.rti,
                 [&](const Transfer& t) {
                     out.insert(key_of(t.type, t.id,
                                       t.source.has_value() ? static_cast<int>(t.source.value()) : -1,
                                       t.dest.has_value() ? static_cast<int>(t.dest.value()) : -1,
                                       t.transfer_id, t.payload));
                 });
    });
    EXPECT_TRUE(status.ok()) << status.ToString();
    return out;
}

std::multiset<Key> expected_fixture(const std::string& transfers_file) {
    auto infile = arrow::io::ReadableFile::Open(transfers_file).ValueOrDie();
    auto reader = arrow::ipc::RecordBatchStreamReader::Open(infile).ValueOrDie();
    auto table_result = reader->ToTable();
    EXPECT_TRUE(table_result.ok());
    std::shared_ptr<arrow::Table> table = table_result.ValueOrDie();
    auto type = std::static_pointer_cast<arrow::StringArray>(table->column(table->schema()->GetFieldIndex("type"))->chunk(0));
    auto id = std::static_pointer_cast<arrow::Int16Array>(table->column(table->schema()->GetFieldIndex("id"))->chunk(0));
    auto src = std::static_pointer_cast<arrow::UInt8Array>(table->column(table->schema()->GetFieldIndex("source"))->chunk(0));
    auto dst = std::static_pointer_cast<arrow::UInt8Array>(table->column(table->schema()->GetFieldIndex("dest"))->chunk(0));
    auto tid = std::static_pointer_cast<arrow::UInt8Array>(table->column(table->schema()->GetFieldIndex("transfer_id"))->chunk(0));
    auto pl = std::static_pointer_cast<arrow::BinaryArray>(table->column(table->schema()->GetFieldIndex("payload"))->chunk(0));
    std::multiset<Key> out;
    for (int64_t i = 0; i < table->num_rows(); ++i) {
        auto view = pl->GetView(i);
        std::vector<uint8_t> payload(view.begin(), view.end());
        out.insert(key_of(type->GetString(i), id->Value(i),
                          src->IsNull(i) ? -1 : src->Value(i),
                          dst->IsNull(i) ? -1 : dst->Value(i),
                          tid->Value(i), payload));
    }
    return out;
}

void check_channel(const std::string& ch) {
    auto got = reassemble_fixture(fixture("frames_" + ch + ".arrows"));
    auto want = expected_fixture(fixture("transfers_" + ch + ".arrows"));
    EXPECT_EQ(got, want) << "channel " << ch << ": reassembled transfers differ from golden";
}
}  // namespace

TEST(Golden, CAN1) { check_channel("CAN1"); }
TEST(Golden, CAN2) { check_channel("CAN2"); }
