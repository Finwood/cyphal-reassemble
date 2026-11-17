#ifndef CYPHAL_REASSEMBLE_ARROW_IO_H
#define CYPHAL_REASSEMBLE_ARROW_IO_H

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <arrow/io/interfaces.h>
#include <arrow/ipc/writer.h>
#include <arrow/status.h>

#include "transfer.h"

struct InputFrame {
    int64_t timestamp_us;
    uint32_t extended_can_id;
    std::vector<uint8_t> data;   // full CAN payload including tail byte
    uint8_t rti;                 // 0 if the input has no `rti` column
};

// Read an Arrow IPC stream of frames from `source`, invoking `sink` per frame.
// Validates the input schema by column name/type. Returns non-ok Status on
// schema or I/O failure.
arrow::Status read_frames(std::shared_ptr<arrow::io::InputStream> source,
                          const std::function<void(const InputFrame&)>& sink);

// Writer for the transfer output stream. Buffers rows and flushes RecordBatches.
class TransferWriter {
public:
    explicit TransferWriter(std::shared_ptr<arrow::io::OutputStream> sink,
                            int64_t batch_size = 10000);
    ~TransferWriter();
    arrow::Status Append(const Transfer& t);
    arrow::Status Close();   // flush remaining rows and close the stream

    static std::shared_ptr<arrow::Schema> schema();

private:
    arrow::Status Flush();

    std::shared_ptr<arrow::io::OutputStream> sink_;
    int64_t batch_size_;
    // builders declared in the .cpp via a pimpl-free member struct
    struct Builders;
    std::unique_ptr<Builders> b_;
    std::shared_ptr<arrow::ipc::RecordBatchWriter> writer_;
    int64_t rows_ = 0;
};

#endif
