#include <cstdio>
#include <memory>

#include <arrow/io/stdio.h>

#include "arrow_io.h"
#include "reassembler.h"

int main() {
    auto in = std::make_shared<arrow::io::StdinStream>();
    auto out = std::make_shared<arrow::io::StdoutStream>();

    Reassembler reassembler;
    TransferWriter writer(out);

    arrow::Status write_status = arrow::Status::OK();
    auto sink = [&](const Transfer& t) {
        if (write_status.ok()) {
            write_status = writer.Append(t);
        }
    };

    auto read_status = read_frames(in, [&](const InputFrame& f) {
        reassembler.ingest(f.timestamp_us, f.extended_can_id, f.data.data(), f.data.size(), f.rti, sink);
    });

    if (!read_status.ok()) {
        std::fprintf(stderr, "error: reading input stream: %s\n", read_status.ToString().c_str());
        return 1;
    }
    if (!write_status.ok()) {
        std::fprintf(stderr, "error: writing output stream: %s\n", write_status.ToString().c_str());
        return 1;
    }

    auto close_status = writer.Close();
    if (!close_status.ok()) {
        std::fprintf(stderr, "error: finalizing output stream: %s\n", close_status.ToString().c_str());
        return 1;
    }

    const auto& s = reassembler.stats();
    std::fprintf(stderr,
                 "cyphal-reassemble: frames_in=%llu transfers_out=%llu errors=%llu oom=%llu\n",
                 static_cast<unsigned long long>(s.frames_in),
                 static_cast<unsigned long long>(s.transfers_out),
                 static_cast<unsigned long long>(s.errors),
                 static_cast<unsigned long long>(s.oom));
    return 0;
}
