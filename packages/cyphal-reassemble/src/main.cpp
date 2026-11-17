#include <cstdio>
#include <cstring>
#include <memory>
#include <unistd.h>

#include <arrow/io/stdio.h>

#include "arrow_io.h"
#include "reassembler.h"
#include "usage.h"

namespace {

bool wants_help(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            return true;
        }
    }
    return false;
}

const char* first_unknown_arg(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") != 0 && std::strcmp(argv[i], "-h") != 0) {
            return argv[i];
        }
    }
    return nullptr;
}

bool stdin_is_interactive() {
    return isatty(fileno(stdin)) != 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (wants_help(argc, argv)) {
        cy_print_usage(stdout);
        return 0;
    }

    if (const char* unknown = first_unknown_arg(argc, argv)) {
        std::fprintf(stderr, "error: unknown argument: %s\n\n", unknown);
        cy_print_usage(stderr);
        return 2;
    }

    // No piped input: user likely launched the binary directly from a shell.
    if (stdin_is_interactive()) {
        cy_print_usage(stdout);
        return 0;
    }

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
