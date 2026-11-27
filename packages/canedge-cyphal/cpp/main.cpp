#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include <arrow/io/api.h>
#include <arrow/io/stdio.h>

#include "arrow_out.h"
#include "decode.h"
#include "mfc.h"

namespace {

std::vector<uint8_t> read_file(const char* path, std::string& err) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        err = std::strerror(errno);
        return {};
    }
    auto size = in.tellg();
    if (size < 0) {
        err = "failed to read file size";
        return {};
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(buf.data()), size)) {
        err = "failed to read file";
        return {};
    }
    return buf;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "error: usage: canedge-mf4 FILE\n";
        return 2;
    }

    std::string err;
    auto file_bytes = read_file(argv[1], err);
    if (!err.empty()) {
        std::cerr << "error: " << err << "\n";
        return 1;
    }

    auto mfc = unwrap_mfc(file_bytes.data(), file_bytes.size());
    if (!mfc.error.empty()) {
        std::cerr << "error: " << mfc.error << "\n";
        return 1;
    }

    auto decoded = decode_mf4(mfc.bytes.data(), mfc.bytes.size());
    if (!decoded.error.empty()) {
        std::cerr << "error: " << decoded.error << "\n";
        return 1;
    }

    auto sink = std::make_shared<arrow::io::StdoutStream>();
    arrow::Status st = write_frame_ipc(decoded.frames, sink);
    if (!st.ok()) {
        std::cerr << "error: " << st.ToString() << "\n";
        return 1;
    }
    return 0;
}