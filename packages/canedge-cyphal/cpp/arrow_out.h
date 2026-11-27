#pragma once

#include <memory>
#include <vector>

#include <arrow/io/interfaces.h>
#include <arrow/status.h>

#include "decode.h"

arrow::Status write_frame_ipc(const std::vector<Frame>& frames,
                              const std::shared_ptr<arrow::io::OutputStream>& sink);