# cyphal-reassemble Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone C++ executable that reassembles Cyphal/CAN v1.0 transfers from an Arrow IPC stream of pre-filtered Cyphal frames using compiled libcanard, replacing the slow pycyphal reassembly step in the frame-decoding pipeline.

**Architecture:** Read an Arrow IPC **stream** from stdin (`timestamp, id, data`, optional `rti`); decode each 29-bit CAN ID to route frames to per-purpose `CanardInstance`s (one message instance + lazy per-destination service instances, to work around libcanard's destination-node gating and achieve promiscuous capture); feed frames to `canardRxAccept` with lazily-created subscriptions; emit completed transfers as an Arrow IPC stream on stdout. Channel/logger/session semantics stay in the Python pipeline.

**Tech Stack:** C++17 + C, libcanard v4.0.0 (vendored git submodule), Apache Arrow C++ (system `libarrow-devel`, `find_package(Arrow)`), CMake, GoogleTest + CTest. Hosted on GitHub; GitHub Actions CI later.

**Reference spec:** `docs/superpowers/specs/2026-07-03-cyphal-reassemble-design.md` (read it first).

---

## Prerequisites

- Fedora dev host. Install Arrow C++ dev headers once:

```bash
sudo dnf install -y libarrow-devel cmake gcc-c++ git
```

- Repo already exists at `/home/lasse/work/canedge/cyphal-reassemble` with `docs/` committed. All paths below are relative to that repo root.

## Cyphal/CAN reference facts (used throughout)

29-bit extended CAN ID bit layout (from libcanard v4.0.0 `canard.c`):

- `priority = (id >> 26) & 0x7`
- `source_node_id = id & 0x7F`
- `FLAG_SERVICE_NOT_MESSAGE = 1 << 25`
- **Message** (`service bit == 0`): `subject_id = (id >> 8) & 0x1FFF`; `FLAG_ANONYMOUS_MESSAGE = 1 << 24` (anonymous ⇒ source = 255/UNSET); destination = UNSET; valid requires reserved bit 23 == 0 and reserved bit 7 == 0.
- **Service** (`service bit == 1`): `FLAG_REQUEST_NOT_RESPONSE = 1 << 24` (set ⇒ Request, clear ⇒ Response); `service_id = (id >> 14) & 0x1FF`; `dest_node_id = (id >> 7) & 0x7F`; valid requires reserved bit 23 == 0 and source != dest.

Tail byte (last payload byte): `SOT = 0x80`, `EOT = 0x40`, `TOGGLE = 0x20`, `transfer_id = tail & 0x1F`.

Verified example CAN IDs (used in tests):

| Meaning | CAN ID | prio | src | kind | port_id | dest |
| --- | --- | --- | --- | --- | --- | --- |
| Heartbeat message, node 42, subject 7509 | `0x101D552A` | 4 | 42 | Message | 7509 | — |
| Service response, service 430, 125→42 | `0x126B957D` | 4 | 125 | Response | 430 | 42 |
| Service request, service 430, 42→125 | `0x136BBEAA` | 4 | 42 | Request | 430 | 125 |

libcanard v4.0.0 RX API (from `canard.h`):

- `struct CanardInstance canardInit(struct CanardMemoryResource memory);` — returns by value; set `.node_id` after (default `CANARD_NODE_ID_UNSET`).
- `struct CanardMemoryResource { void* user_reference; CanardMemoryDeallocate deallocate; CanardMemoryAllocate allocate; };`
  - `typedef void* (*CanardMemoryAllocate)(void* user_reference, size_t size);`
  - `typedef void (*CanardMemoryDeallocate)(void* user_reference, size_t size, void* pointer);`
- `int8_t canardRxSubscribe(CanardInstance*, CanardTransferKind, CanardPortID port_id, size_t extent, CanardMicrosecond tid_timeout_usec, CanardRxSubscription* out_sub);` — `out_sub` storage must stay put (never moved) while in use.
- `int8_t canardRxAccept(CanardInstance*, CanardMicrosecond ts_usec, const CanardFrame*, uint8_t redundant_iface_index, CanardRxTransfer* out_transfer, CanardRxSubscription** out_sub);` — returns `1` if a transfer completed, `0` if not, negative on OOM/invalid-arg. On `1`, caller MUST free `out_transfer.payload.data` via the instance's memory resource using `payload.allocated_size`.
- `struct CanardFrame { uint32_t extended_can_id; struct CanardPayload payload; };` with `struct CanardPayload { size_t size; const void* data; };` — pass the **full** CAN data including the tail byte; libcanard strips it internally.
- `struct CanardRxTransfer { struct CanardTransferMetadata metadata; CanardMicrosecond timestamp_usec; struct CanardMutablePayload payload; };`
- `struct CanardTransferMetadata { enum CanardPriority priority; enum CanardTransferKind transfer_kind; CanardPortID port_id; CanardNodeID remote_node_id; CanardTransferID transfer_id; };`
- `enum CanardTransferKind { CanardTransferKindMessage=0, CanardTransferKindResponse=1, CanardTransferKindRequest=2 };`
- Constants: `CANARD_NODE_ID_UNSET=255`, `CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC=2000000`.

---

## File Structure

| File | Responsibility |
| --- | --- |
| `CMakeLists.txt` | Build: Arrow discovery, libcanard static lib, core static lib, executable, GoogleTest/CTest |
| `third_party/libcanard/` | Vendored libcanard v4.0.0 (git submodule; source at `libcanard/canard.c`, header `libcanard/canard.h`) |
| `src/can_id.h`, `src/can_id.c` | Pure C decode of a 29-bit CAN ID → `{valid, kind, port_id, priority, source, dest}` |
| `src/transfer.h` | `Transfer` POD struct: the 9 output fields (interface between reassembler and arrow_io) |
| `src/reassembler.h`, `src/reassembler.cpp` | libcanard wrapper: message + lazy per-dest service instances, lazy subscriptions, malloc/free memory resource, ingest→emit |
| `src/arrow_io.h`, `src/arrow_io.cpp` | Arrow IPC stream read + input schema validation; output schema, builders, batched stream write |
| `src/main.cpp` | CLI: stdin→reassembler→stdout wiring, stderr summary counts, exit codes |
| `test/test_can_id.cpp` | Unit tests for `cy_decode_can_id` |
| `test/test_reassembler.cpp` | Unit tests for single-frame reassembly + promiscuous service routing |
| `test/test_arrow_io.cpp` | Unit tests for input schema validation + output round-trip |
| `test/test_golden.cpp` | End-to-end parity test against vendored fixtures |
| `tools/export_fixtures.py` | Script (run in pipeline env) to generate vendored fixtures |
| `tests/data/` | Vendored `.arrows` fixtures |
| `README.md`, `LICENSE` | Docs + MIT license |
| `.github/workflows/ci.yml` | GitHub Actions build+test |

---

## Task 1: Repo scaffold and smoke build

**Files:**
- Create: `LICENSE`, `CMakeLists.txt`, `src/main.cpp`, `test/test_smoke.cpp`
- Submodule: `third_party/libcanard`

- [ ] **Step 1: Add libcanard v4.0.0 as a submodule**

Run:
```bash
cd /home/lasse/work/canedge/cyphal-reassemble
git submodule add https://github.com/OpenCyphal/libcanard.git third_party/libcanard
git -C third_party/libcanard checkout v4.0.0
git add .gitmodules third_party/libcanard
```
Expected: `third_party/libcanard/libcanard/canard.c` and `canard.h` exist.

- [ ] **Step 2: Add the MIT LICENSE**

Create `LICENSE`:
```text
MIT License

Copyright (c) 2026 Lasse Fröhner

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

- [ ] **Step 3: Write the smoke test**

Create `test/test_smoke.cpp`:
```cpp
#include <gtest/gtest.h>

TEST(Smoke, TrueIsTrue) {
    EXPECT_TRUE(true);
}
```

- [ ] **Step 4: Write a placeholder `main.cpp`**

Create `src/main.cpp`:
```cpp
int main() {
    return 0;
}
```

- [ ] **Step 5: Write `CMakeLists.txt`**

Create `CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
project(cyphal_reassemble LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Arrow REQUIRED)

# Vendored libcanard (C)
add_library(canard STATIC third_party/libcanard/libcanard/canard.c)
target_include_directories(canard PUBLIC third_party/libcanard/libcanard)

# Core library (added to as the project grows)
add_library(reassemble_core STATIC src/main_placeholder.c)
target_include_directories(reassemble_core PUBLIC src)
target_link_libraries(reassemble_core PUBLIC canard Arrow::arrow_shared)

add_executable(cyphal-reassemble src/main.cpp)
target_link_libraries(cyphal-reassemble PRIVATE reassemble_core)

# Tests
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(unit_tests test/test_smoke.cpp)
target_link_libraries(unit_tests PRIVATE reassemble_core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(unit_tests)
```

Create `src/main_placeholder.c` (temporary, replaced in Task 2 by real sources):
```c
/* Placeholder so reassemble_core has a source until real files are added. */
int cyphal_reassemble_placeholder(void) { return 0; }
```

- [ ] **Step 6: Configure, build, and run the smoke test**

Run:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```
Expected: configuration finds Arrow, build succeeds, `Smoke.TrueIsTrue` passes.

- [ ] **Step 7: Add `.gitignore` entry check and commit**

`.gitignore` already exists (ignores `/build/`). Run:
```bash
git add -A
git commit -m "chore: scaffold CMake build with libcanard submodule and GoogleTest"
```

---

## Task 2: CAN ID decoder

**Files:**
- Create: `src/can_id.h`, `src/can_id.c`, `test/test_can_id.cpp`
- Modify: `CMakeLists.txt` (add source + test)

- [ ] **Step 1: Write the failing test**

Create `test/test_can_id.cpp`:
```cpp
#include <gtest/gtest.h>
extern "C" {
#include "can_id.h"
}

TEST(CanId, HeartbeatMessage) {
    cy_can_id_t d = cy_decode_can_id(0x101D552AU);
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.kind, CY_KIND_MESSAGE);
    EXPECT_EQ(d.port_id, 7509);
    EXPECT_EQ(d.priority, 4);
    EXPECT_EQ(d.source_node_id, 42);
    EXPECT_EQ(d.dest_node_id, CY_NODE_ID_UNSET);
}

TEST(CanId, ServiceResponse) {
    cy_can_id_t d = cy_decode_can_id(0x126B957DU);
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.kind, CY_KIND_RESPONSE);
    EXPECT_EQ(d.port_id, 430);
    EXPECT_EQ(d.source_node_id, 125);
    EXPECT_EQ(d.dest_node_id, 42);
}

TEST(CanId, ServiceRequest) {
    cy_can_id_t d = cy_decode_can_id(0x136BBEAAU);
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.kind, CY_KIND_REQUEST);
    EXPECT_EQ(d.port_id, 430);
    EXPECT_EQ(d.source_node_id, 42);
    EXPECT_EQ(d.dest_node_id, 125);
}

TEST(CanId, AnonymousMessageHasUnsetSource) {
    // Heartbeat layout with the anonymous flag (bit 24) set.
    cy_can_id_t d = cy_decode_can_id(0x101D552AU | (1U << 24));
    EXPECT_TRUE(d.valid);
    EXPECT_EQ(d.kind, CY_KIND_MESSAGE);
    EXPECT_EQ(d.source_node_id, CY_NODE_ID_UNSET);
}

TEST(CanId, ServiceWithEqualSourceAndDestIsInvalid) {
    // service 430, dest 42, source 42 -> invalid per spec
    uint32_t id = (4U << 26) | (1U << 25) | (430U << 14) | (42U << 7) | 42U;
    cy_can_id_t d = cy_decode_can_id(id);
    EXPECT_FALSE(d.valid);
}

TEST(CanId, MessageWithReservedBit7IsInvalid) {
    cy_can_id_t d = cy_decode_can_id(0x101D552AU | (1U << 7));
    EXPECT_FALSE(d.valid);
}
```

- [ ] **Step 2: Add the header**

Create `src/can_id.h`:
```c
#ifndef CYPHAL_REASSEMBLE_CAN_ID_H
#define CYPHAL_REASSEMBLE_CAN_ID_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CY_NODE_ID_UNSET 255U

typedef enum {
    CY_KIND_MESSAGE = 0,
    CY_KIND_RESPONSE = 1,
    CY_KIND_REQUEST = 2
} cy_transfer_kind_t;

typedef struct {
    bool valid;                 /* false if the ID is not a valid Cyphal/CAN frame layout */
    cy_transfer_kind_t kind;
    uint16_t port_id;           /* subject-id (message) or service-id (service) */
    uint8_t priority;           /* 0..7 */
    uint8_t source_node_id;     /* 0..127, or CY_NODE_ID_UNSET if anonymous */
    uint8_t dest_node_id;       /* 0..127 for services; CY_NODE_ID_UNSET for messages */
} cy_can_id_t;

/* Decode a 29-bit extended CAN ID into its Cyphal/CAN fields. */
cy_can_id_t cy_decode_can_id(uint32_t extended_can_id);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 3: Add the implementation**

Create `src/can_id.c`:
```c
#include "can_id.h"

#define OFFSET_PRIORITY 26U
#define OFFSET_SUBJECT_ID 8U
#define OFFSET_SERVICE_ID 14U
#define OFFSET_DST_NODE_ID 7U

#define FLAG_SERVICE_NOT_MESSAGE  (UINT32_C(1) << 25U)
#define FLAG_ANONYMOUS_MESSAGE    (UINT32_C(1) << 24U)
#define FLAG_REQUEST_NOT_RESPONSE (UINT32_C(1) << 24U)
#define FLAG_RESERVED_23          (UINT32_C(1) << 23U)
#define FLAG_RESERVED_07          (UINT32_C(1) << 7U)

#define PRIORITY_MAX 7U
#define NODE_ID_MAX 127U
#define SUBJECT_ID_MAX 8191U
#define SERVICE_ID_MAX 511U

cy_can_id_t cy_decode_can_id(uint32_t can_id)
{
    cy_can_id_t out;
    out.valid = false;
    out.kind = CY_KIND_MESSAGE;
    out.port_id = 0U;
    out.priority = (uint8_t)((can_id >> OFFSET_PRIORITY) & PRIORITY_MAX);
    out.source_node_id = (uint8_t)(can_id & NODE_ID_MAX);
    out.dest_node_id = CY_NODE_ID_UNSET;

    if (0U == (can_id & FLAG_SERVICE_NOT_MESSAGE)) {
        out.kind = CY_KIND_MESSAGE;
        out.port_id = (uint16_t)((can_id >> OFFSET_SUBJECT_ID) & SUBJECT_ID_MAX);
        if ((can_id & FLAG_ANONYMOUS_MESSAGE) != 0U) {
            out.source_node_id = (uint8_t)CY_NODE_ID_UNSET;
        }
        out.dest_node_id = (uint8_t)CY_NODE_ID_UNSET;
        out.valid = (0U == (can_id & FLAG_RESERVED_23)) && (0U == (can_id & FLAG_RESERVED_07));
    } else {
        out.kind = ((can_id & FLAG_REQUEST_NOT_RESPONSE) != 0U) ? CY_KIND_REQUEST : CY_KIND_RESPONSE;
        out.port_id = (uint16_t)((can_id >> OFFSET_SERVICE_ID) & SERVICE_ID_MAX);
        out.dest_node_id = (uint8_t)((can_id >> OFFSET_DST_NODE_ID) & NODE_ID_MAX);
        out.valid = (0U == (can_id & FLAG_RESERVED_23)) && (out.source_node_id != out.dest_node_id);
    }
    return out;
}
```

- [ ] **Step 4: Wire sources into CMake**

In `CMakeLists.txt`, replace the `reassemble_core` library definition and the `unit_tests` executable line:
```cmake
add_library(reassemble_core STATIC
    src/can_id.c)
target_include_directories(reassemble_core PUBLIC src)
target_link_libraries(reassemble_core PUBLIC canard Arrow::arrow_shared)
```
```cmake
add_executable(unit_tests
    test/test_smoke.cpp
    test/test_can_id.cpp)
target_link_libraries(unit_tests PRIVATE reassemble_core GTest::gtest_main)
```
Then delete `src/main_placeholder.c`:
```bash
git rm src/main_placeholder.c
```

- [ ] **Step 5: Build and run tests**

Run:
```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```
Expected: all `CanId.*` tests pass.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add Cyphal/CAN ID decoder"
```

---

## Task 3: Transfer struct and reassembler skeleton

**Files:**
- Create: `src/transfer.h`, `src/reassembler.h`, `src/reassembler.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add the `Transfer` struct**

Create `src/transfer.h`:
```cpp
#ifndef CYPHAL_REASSEMBLE_TRANSFER_H
#define CYPHAL_REASSEMBLE_TRANSFER_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// One reassembled Cyphal transfer. Mirrors sc_schema.TransferSchema minus `channel`.
struct Transfer {
    int64_t timestamp_us = 0;              // microseconds since UNIX epoch, UTC (first frame)
    std::string type;                      // "Message" | "Request" | "Response"
    int16_t id = 0;                        // subject-id or service-id
    std::optional<uint8_t> source;         // origin node-id; empty if anonymous
    std::optional<uint8_t> dest;           // destination node-id; empty for messages
    uint8_t priority = 0;                  // 0..7
    uint8_t transfer_id = 0;               // 0..31
    std::vector<uint8_t> payload;
    int32_t length = 0;                    // payload.size()
};

#endif
```

- [ ] **Step 2: Add the reassembler header**

Create `src/reassembler.h`:
```cpp
#ifndef CYPHAL_REASSEMBLE_REASSEMBLER_H
#define CYPHAL_REASSEMBLE_REASSEMBLER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <tuple>

extern "C" {
#include "canard.h"
}
#include "transfer.h"

struct ReassemblerStats {
    uint64_t frames_in = 0;
    uint64_t transfers_out = 0;
    uint64_t errors = 0;   // invalid frame layout, skipped
    uint64_t oom = 0;      // canardRxAccept out-of-memory, skipped
};

class Reassembler {
public:
    using TransferSink = std::function<void(const Transfer&)>;

    explicit Reassembler(std::size_t extent = 65535U,
                         CanardMicrosecond transfer_id_timeout_us = CANARD_DEFAULT_TRANSFER_ID_TIMEOUT_USEC);
    ~Reassembler();

    Reassembler(const Reassembler&) = delete;
    Reassembler& operator=(const Reassembler&) = delete;

    // Feed one CAN frame; completed transfers are delivered to `sink`.
    void ingest(int64_t timestamp_us, uint32_t extended_can_id,
                const uint8_t* data, std::size_t size, uint8_t rti,
                const TransferSink& sink);

    const ReassemblerStats& stats() const { return stats_; }

private:
    CanardInstance& message_instance();
    CanardInstance& service_instance(uint8_t dest_node_id);
    void ensure_subscription(CanardInstance& ins, CanardTransferKind kind, CanardPortID port_id);

    std::size_t extent_;
    CanardMicrosecond tid_timeout_us_;
    ReassemblerStats stats_;

    std::unique_ptr<CanardInstance> message_;
    std::map<uint8_t, std::unique_ptr<CanardInstance>> services_;
    // Subscriptions must never move while in use; own them here, keyed by (instance, kind, port_id).
    std::map<std::tuple<CanardInstance*, int, uint16_t>, std::unique_ptr<CanardRxSubscription>> subscriptions_;
};

#endif
```

- [ ] **Step 3: Add the reassembler implementation (skeleton: memory + instances + subscriptions; `ingest` filled in Task 4)**

Create `src/reassembler.cpp`:
```cpp
#include "reassembler.h"

#include <cstdlib>

extern "C" {
#include "can_id.h"
}

namespace {
void* mem_allocate(void* /*user*/, std::size_t size) { return std::malloc(size); }
void mem_deallocate(void* /*user*/, std::size_t /*size*/, void* ptr) { std::free(ptr); }

CanardMemoryResource make_memory() {
    CanardMemoryResource m;
    m.user_reference = nullptr;
    m.deallocate = &mem_deallocate;
    m.allocate = &mem_allocate;
    return m;
}
}  // namespace

Reassembler::Reassembler(std::size_t extent, CanardMicrosecond transfer_id_timeout_us)
    : extent_(extent), tid_timeout_us_(transfer_id_timeout_us) {}

Reassembler::~Reassembler() = default;

CanardInstance& Reassembler::message_instance() {
    if (!message_) {
        message_ = std::make_unique<CanardInstance>(canardInit(make_memory()));
        message_->node_id = CANARD_NODE_ID_UNSET;  // anonymous; accepts all broadcast messages
    }
    return *message_;
}

CanardInstance& Reassembler::service_instance(uint8_t dest_node_id) {
    auto it = services_.find(dest_node_id);
    if (it == services_.end()) {
        auto ins = std::make_unique<CanardInstance>(canardInit(make_memory()));
        ins->node_id = dest_node_id;  // so canardRxAccept accepts services addressed to this dest
        it = services_.emplace(dest_node_id, std::move(ins)).first;
    }
    return *it->second;
}

void Reassembler::ensure_subscription(CanardInstance& ins, CanardTransferKind kind, CanardPortID port_id) {
    auto key = std::make_tuple(&ins, static_cast<int>(kind), static_cast<uint16_t>(port_id));
    if (subscriptions_.count(key) != 0) {
        return;
    }
    auto sub = std::make_unique<CanardRxSubscription>();
    (void)canardRxSubscribe(&ins, kind, port_id, extent_, tid_timeout_us_, sub.get());
    subscriptions_.emplace(std::move(key), std::move(sub));
}

void Reassembler::ingest(int64_t /*timestamp_us*/, uint32_t /*extended_can_id*/,
                         const uint8_t* /*data*/, std::size_t /*size*/, uint8_t /*rti*/,
                         const TransferSink& /*sink*/) {
    // Implemented in Task 4.
}
```

- [ ] **Step 4: Wire sources into CMake**

In `CMakeLists.txt`, update `reassemble_core` sources:
```cmake
add_library(reassemble_core STATIC
    src/can_id.c
    src/reassembler.cpp)
target_include_directories(reassemble_core PUBLIC src)
target_link_libraries(reassemble_core PUBLIC canard Arrow::arrow_shared)
```

- [ ] **Step 5: Build (no new tests yet)**

Run:
```bash
cmake --build build -j
```
Expected: builds cleanly (the reassembler compiles and links against libcanard).

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add reassembler skeleton (instances, subscriptions, memory)"
```

---

## Task 4: Reassembler ingest and transfer conversion

**Files:**
- Modify: `src/reassembler.cpp`
- Create: `test/test_reassembler.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `test/test_reassembler.cpp`:
```cpp
#include <gtest/gtest.h>

#include <vector>

#include "reassembler.h"

namespace {
// Build a single-frame CAN payload: data bytes followed by a tail byte
// with SOT|EOT|TOGGLE set and the given transfer_id.
std::vector<uint8_t> single_frame(std::vector<uint8_t> data, uint8_t transfer_id) {
    uint8_t tail = 0x80U | 0x40U | 0x20U | (transfer_id & 0x1FU);
    data.push_back(tail);
    return data;
}

std::vector<Transfer> collect(Reassembler& r, int64_t ts, uint32_t can_id,
                              const std::vector<uint8_t>& payload) {
    std::vector<Transfer> out;
    r.ingest(ts, can_id, payload.data(), payload.size(), 0U,
             [&](const Transfer& t) { out.push_back(t); });
    return out;
}
}  // namespace

TEST(Reassembler, SingleFrameHeartbeatMessage) {
    Reassembler r;
    auto frame = single_frame({0x55, 0x02, 0x00, 0x00, 0x00, 0x00, 0x09}, 21);
    auto out = collect(r, 1000, 0x101D552AU, frame);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].type, "Message");
    EXPECT_EQ(out[0].id, 7509);
    ASSERT_TRUE(out[0].source.has_value());
    EXPECT_EQ(out[0].source.value(), 42);
    EXPECT_FALSE(out[0].dest.has_value());
    EXPECT_EQ(out[0].priority, 4);
    EXPECT_EQ(out[0].transfer_id, 21);
    EXPECT_EQ(out[0].length, 7);
    EXPECT_EQ(out[0].payload, (std::vector<uint8_t>{0x55, 0x02, 0x00, 0x00, 0x00, 0x00, 0x09}));
    EXPECT_EQ(r.stats().frames_in, 1U);
    EXPECT_EQ(r.stats().transfers_out, 1U);
}

TEST(Reassembler, SingleFrameServiceResponseIsCapturedPromiscuously) {
    Reassembler r;
    auto frame = single_frame({0xAA, 0xBB}, 3);
    auto out = collect(r, 2000, 0x126B957DU, frame);  // service 430, 125 -> 42
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].type, "Response");
    EXPECT_EQ(out[0].id, 430);
    ASSERT_TRUE(out[0].source.has_value());
    EXPECT_EQ(out[0].source.value(), 125);
    ASSERT_TRUE(out[0].dest.has_value());
    EXPECT_EQ(out[0].dest.value(), 42);
    EXPECT_EQ(out[0].transfer_id, 3);
    EXPECT_EQ(out[0].payload, (std::vector<uint8_t>{0xAA, 0xBB}));
}

TEST(Reassembler, SingleFrameServiceRequestToDifferentDest) {
    Reassembler r;
    auto frame = single_frame({0x01}, 7);
    auto out = collect(r, 3000, 0x136BBEAAU, frame);  // service 430, 42 -> 125
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].type, "Request");
    EXPECT_EQ(out[0].id, 430);
    ASSERT_TRUE(out[0].source.has_value());
    EXPECT_EQ(out[0].source.value(), 42);
    ASSERT_TRUE(out[0].dest.has_value());
    EXPECT_EQ(out[0].dest.value(), 125);
    EXPECT_EQ(out[0].transfer_id, 7);
}

TEST(Reassembler, InvalidFrameLayoutIsCountedAsError) {
    Reassembler r;
    // Service frame with source == dest is invalid.
    uint32_t id = (4U << 26) | (1U << 25) | (430U << 14) | (42U << 7) | 42U;
    auto frame = single_frame({0x01}, 1);
    auto out = collect(r, 4000, id, frame);
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(r.stats().errors, 1U);
    EXPECT_EQ(r.stats().transfers_out, 0U);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
cmake --build build -j && ctest --test-dir build -R Reassembler --output-on-failure
```
Expected: FAILS — `ingest` currently does nothing, so no transfers are emitted.

- [ ] **Step 3: Implement `ingest` and conversion**

In `src/reassembler.cpp`, replace the placeholder `ingest` body with:
```cpp
void Reassembler::ingest(int64_t timestamp_us, uint32_t extended_can_id,
                         const uint8_t* data, std::size_t size, uint8_t rti,
                         const TransferSink& sink) {
    stats_.frames_in++;

    const cy_can_id_t decoded = cy_decode_can_id(extended_can_id);
    if (!decoded.valid) {
        stats_.errors++;
        return;
    }

    CanardInstance* ins = nullptr;
    CanardTransferKind kind = CanardTransferKindMessage;
    if (decoded.kind == CY_KIND_MESSAGE) {
        ins = &message_instance();
        kind = CanardTransferKindMessage;
    } else {
        ins = &service_instance(decoded.dest_node_id);
        kind = (decoded.kind == CY_KIND_REQUEST) ? CanardTransferKindRequest : CanardTransferKindResponse;
    }

    ensure_subscription(*ins, kind, decoded.port_id);

    CanardFrame frame;
    frame.extended_can_id = extended_can_id;
    frame.payload.size = size;
    frame.payload.data = data;

    CanardRxTransfer transfer;
    CanardRxSubscription* sub = nullptr;
    const int8_t result = canardRxAccept(ins, static_cast<CanardMicrosecond>(timestamp_us),
                                         &frame, rti, &transfer, &sub);
    if (result < 0) {
        stats_.oom++;
        return;
    }
    if (result != 1) {
        return;  // frame accepted but transfer not yet complete
    }

    Transfer out;
    out.timestamp_us = static_cast<int64_t>(transfer.timestamp_usec);
    switch (transfer.metadata.transfer_kind) {
        case CanardTransferKindMessage: out.type = "Message"; break;
        case CanardTransferKindRequest: out.type = "Request"; break;
        case CanardTransferKindResponse: out.type = "Response"; break;
        default: out.type = "Message"; break;
    }
    out.id = static_cast<int16_t>(transfer.metadata.port_id);
    if (transfer.metadata.remote_node_id == CANARD_NODE_ID_UNSET) {
        out.source = std::nullopt;
    } else {
        out.source = static_cast<uint8_t>(transfer.metadata.remote_node_id);
    }
    if (transfer.metadata.transfer_kind == CanardTransferKindMessage) {
        out.dest = std::nullopt;
    } else {
        out.dest = static_cast<uint8_t>(ins->node_id);
    }
    out.priority = static_cast<uint8_t>(transfer.metadata.priority);
    out.transfer_id = static_cast<uint8_t>(transfer.metadata.transfer_id);
    const auto* bytes = static_cast<const uint8_t*>(transfer.payload.data);
    out.payload.assign(bytes, bytes + transfer.payload.size);
    out.length = static_cast<int32_t>(transfer.payload.size);

    // Release the payload buffer allocated by libcanard.
    ins->memory.deallocate(ins->memory.user_reference, transfer.payload.allocated_size,
                           transfer.payload.data);

    stats_.transfers_out++;
    sink(out);
}
```

- [ ] **Step 4: Add the test to CMake**

In `CMakeLists.txt`, add to `unit_tests`:
```cmake
add_executable(unit_tests
    test/test_smoke.cpp
    test/test_can_id.cpp
    test/test_reassembler.cpp)
target_link_libraries(unit_tests PRIVATE reassemble_core GTest::gtest_main)
```

- [ ] **Step 5: Run the tests to verify they pass**

Run:
```bash
cmake --build build -j && ctest --test-dir build -R Reassembler --output-on-failure
```
Expected: all `Reassembler.*` tests pass.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: implement frame ingest and transfer reassembly"
```

---

## Task 5: Arrow input reader with schema validation

**Files:**
- Create: `src/arrow_io.h`, `src/arrow_io.cpp`, `test/test_arrow_io.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `test/test_arrow_io.cpp`:
```cpp
#include <gtest/gtest.h>

#include <arrow/api.h>
#include <arrow/io/memory.h>
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
```

- [ ] **Step 2: Add the header**

Create `src/arrow_io.h`:
```cpp
#ifndef CYPHAL_REASSEMBLE_ARROW_IO_H
#define CYPHAL_REASSEMBLE_ARROW_IO_H

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <arrow/io/interfaces.h>
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
```

- [ ] **Step 3: Add the input implementation (writer added in Task 6)**

Create `src/arrow_io.cpp`:
```cpp
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
```

- [ ] **Step 4: Wire into CMake**

In `CMakeLists.txt`, add `src/arrow_io.cpp` to `reassemble_core` and `test/test_arrow_io.cpp` to `unit_tests`:
```cmake
add_library(reassemble_core STATIC
    src/can_id.c
    src/reassembler.cpp
    src/arrow_io.cpp)
target_include_directories(reassemble_core PUBLIC src)
target_link_libraries(reassemble_core PUBLIC canard Arrow::arrow_shared)
```
```cmake
add_executable(unit_tests
    test/test_smoke.cpp
    test/test_can_id.cpp
    test/test_reassembler.cpp
    test/test_arrow_io.cpp)
target_link_libraries(unit_tests PRIVATE reassemble_core GTest::gtest_main)
```

- [ ] **Step 5: Run the input tests to verify they pass**

Run:
```bash
cmake --build build -j && ctest --test-dir build -R ArrowInput --output-on-failure
```
Expected: `ArrowInput.ReadsFramesFromStream` and `ArrowInput.RejectsMissingRequiredColumn` pass.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: add Arrow IPC frame reader with schema validation"
```

---

## Task 6: Arrow output writer

**Files:**
- Modify: `src/arrow_io.cpp`, `test/test_arrow_io.cpp`

- [ ] **Step 1: Write the failing round-trip test**

Append to `test/test_arrow_io.cpp`:
```cpp
#include <arrow/ipc/reader.h>

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

    std::shared_ptr<arrow::Table> table;
    ASSERT_TRUE(reader->ReadAll(&table).ok());
    ASSERT_EQ(table->num_rows(), 2);

    auto type_col = std::static_pointer_cast<arrow::StringArray>(table->column(1)->chunk(0));
    auto dest_col = std::static_pointer_cast<arrow::UInt8Array>(table->column(3 + 1)->chunk(0));
    EXPECT_EQ(type_col->GetString(0), "Message");
    EXPECT_TRUE(dest_col->IsNull(0));      // message has no destination
    EXPECT_FALSE(dest_col->IsNull(1));     // response has a destination
    EXPECT_EQ(dest_col->Value(1), 42);
}
```

- [ ] **Step 2: Run to verify it fails (link error / unimplemented)**

Run:
```bash
cmake --build build -j
```
Expected: FAILS to link — `TransferWriter` members are not yet defined.

- [ ] **Step 3: Implement `TransferWriter`**

Append to `src/arrow_io.cpp`:
```cpp
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
```

- [ ] **Step 4: Run the output test to verify it passes**

Run:
```bash
cmake --build build -j && ctest --test-dir build -R ArrowOutput --output-on-failure
```
Expected: `ArrowOutput.WritesTransfersRoundTrip` passes.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat: add Arrow IPC transfer writer"
```

---

## Task 7: CLI wiring (main)

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Implement `main`**

Replace `src/main.cpp` with:
```cpp
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
```

- [ ] **Step 2: Build**

Run:
```bash
cmake --build build -j
```
Expected: `build/cyphal-reassemble` builds and links.

- [ ] **Step 3: Manual end-to-end smoke via Python (pyarrow) if available, else skip to Task 8**

Run:
```bash
uv run --with pyarrow python - <<'PY'
import subprocess, io
import pyarrow as pa
schema = pa.schema([
    ("timestamp", pa.timestamp("us", tz="UTC")),
    ("id", pa.uint32()),
    ("data", pa.binary()),
])
batch = pa.record_batch([
    pa.array([1000], pa.timestamp("us", tz="UTC")),
    pa.array([0x101D552A], pa.uint32()),
    pa.array([bytes([0x55,0x02,0x00,0x00,0x00,0x00,0x09,0xF5])], pa.binary()),
], schema=schema)
buf = io.BytesIO()
with pa.ipc.new_stream(buf, schema) as w:
    w.write_batch(batch)
p = subprocess.run(["build/cyphal-reassemble"], input=buf.getvalue(), capture_output=True)
print("stderr:", p.stderr.decode())
reader = pa.ipc.open_stream(io.BytesIO(p.stdout))
print(reader.read_all().to_pydict())
PY
```
Expected: stderr shows `frames_in=1 transfers_out=1 errors=0 oom=0`; the table has one row with `type='Message'`, `id=7509`, `source=42`, `dest=None`, `transfer_id=21`.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: wire stdin->reassembler->stdout CLI with stderr summary"
```

---

## Task 8: End-to-end golden/parity test with vendored fixtures

**Files:**
- Create: `tools/export_fixtures.py`, `test/test_golden.cpp`, `tests/data/frames_CAN1.arrows`, `tests/data/frames_CAN2.arrows`, `tests/data/transfers_CAN1.arrows`, `tests/data/transfers_CAN2.arrows`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add the fixture export script**

Create `tools/export_fixtures.py`:
```python
"""Export golden fixtures for cyphal-reassemble from the frame-decoding pipeline.

Run inside the pipeline repo's Poetry environment, e.g.:

    cd /home/lasse/work/canedge/frame-decoding-pipeline
    poetry run python /home/lasse/work/canedge/cyphal-reassemble/tools/export_fixtures.py \
        --out /home/lasse/work/canedge/cyphal-reassemble/tests/data

For each channel it writes:
  - frames_<CH>.arrows    : input frames (timestamp, id, data), Cyphal-only, ordered
  - transfers_<CH>.arrows : expected transfers (TransferSchema minus channel)
"""

import argparse
from pathlib import Path

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

from frame_decoding_pipeline import db

SAMPLE_LOGGER = "3544BCD3"
SAMPLE_SESSION = 509

PIPELINE_ROOT = Path(__file__).resolve().parents[1]  # adjust if needed


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--frames-hive", type=Path,
                    default=Path("test/data/frames"))
    ap.add_argument("--transfers-parquet", type=Path,
                    default=Path("test/data/transfers/logger=3544BCD3/session=00000509/transfers.parquet"))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    # Expected transfers (golden), split per channel, channel column dropped.
    transfers = pq.read_table(args.transfers_parquet)
    channels = sorted(set(transfers.column("channel").to_pylist()))

    with db.connection() as conn:
        reader = db.get_cyphal_frame_batches(conn, SAMPLE_LOGGER, SAMPLE_SESSION,
                                             hive=args.frames_hive)
        frames = reader.read_all()

    for ch in channels:
        # ---- input frames for this channel ----
        mask = pc.equal(frames.column("channel"), pa.scalar(ch))
        ch_frames = frames.filter(mask).select(["timestamp", "id", "data"])
        ch_frames = ch_frames.sort_by("timestamp")
        in_path = args.out / f"frames_{ch}.arrows"
        with pa.ipc.new_stream(in_path, ch_frames.schema) as w:
            for b in ch_frames.to_batches():
                w.write_batch(b)

        # ---- expected transfers for this channel ----
        tmask = pc.equal(transfers.column("channel"), pa.scalar(ch))
        ch_tr = transfers.filter(tmask).drop_columns(["channel"])
        out_path = args.out / f"transfers_{ch}.arrows"
        with pa.ipc.new_stream(out_path, ch_tr.schema) as w:
            for b in ch_tr.to_batches():
                w.write_batch(b)

        print(f"{ch}: {ch_frames.num_rows} frames -> {ch_tr.num_rows} expected transfers")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Generate the fixtures**

Run (in the pipeline repo, with its Poetry env):
```bash
cd /home/lasse/work/canedge/frame-decoding-pipeline
poetry run python /home/lasse/work/canedge/cyphal-reassemble/tools/export_fixtures.py \
    --out /home/lasse/work/canedge/cyphal-reassemble/tests/data
```
Expected: prints per-channel counts and writes `frames_CAN1.arrows`, `frames_CAN2.arrows`, `transfers_CAN1.arrows`, `transfers_CAN2.arrows`.

Note: `db.connection()` builds a temp DuckDB and reads `test/data/frames`; the sample fixtures already committed in the pipeline are sufficient (no external data needed).

- [ ] **Step 3: Write the golden test**

Create `test/test_golden.cpp`:
```cpp
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
                     out.insert(key_of(t.type, t.id, t.source.value_or(-1), t.dest.value_or(-1),
                                       t.transfer_id, t.payload));
                 });
    });
    EXPECT_TRUE(status.ok()) << status.ToString();
    return out;
}

std::multiset<Key> expected_fixture(const std::string& transfers_file) {
    auto infile = arrow::io::ReadableFile::Open(transfers_file).ValueOrDie();
    auto reader = arrow::ipc::RecordBatchStreamReader::Open(infile).ValueOrDie();
    std::shared_ptr<arrow::Table> table;
    EXPECT_TRUE(reader->ReadAll(&table).ok());
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
```

- [ ] **Step 4: Wire the golden test into CMake with the fixture path**

In `CMakeLists.txt`, add a separate test target so the fixture directory can be injected:
```cmake
add_executable(golden_tests test/test_golden.cpp)
target_link_libraries(golden_tests PRIVATE reassemble_core GTest::gtest_main)
target_compile_definitions(golden_tests PRIVATE
    FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
gtest_discover_tests(golden_tests)
```

- [ ] **Step 5: Build and run the golden tests**

Run:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j && ctest --test-dir build -R Golden --output-on-failure
```
Expected: `Golden.CAN1` and `Golden.CAN2` pass. If a mismatch is only in row count due to multi-frame transfers spanning the transfer-ID timeout, inspect with the stderr counts and the spec's "Open risks" notes; a genuine payload/id/source/dest/transfer_id mismatch is a real bug to fix.

- [ ] **Step 6: Commit (including the vendored `.arrows` fixtures)**

```bash
git add -A
git commit -m "test: add end-to-end golden parity test with vendored fixtures"
```

---

## Task 9: README and GitHub Actions CI

**Files:**
- Create: `README.md`, `.github/workflows/ci.yml`

- [ ] **Step 1: Write the README**

Create `README.md`:
````markdown
# cyphal-reassemble

Standalone reassembler of Cyphal/CAN v1.0 transfers, built on
[libcanard](https://github.com/OpenCyphal/libcanard). Reads an Arrow IPC
**stream** of pre-filtered Cyphal frames from stdin and writes an Arrow IPC
**stream** of reassembled transfers to stdout.

Designed as a fast, compiled replacement for the pycyphal-based reassembly step
of the frame-decoding pipeline. It is deliberately channel/logger/session
agnostic: all such semantics stay in the calling pipeline.

## Build

```bash
sudo dnf install -y libarrow-devel cmake gcc-c++ git   # Fedora
git clone --recurse-submodules <repo-url>
cd cyphal-reassemble
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Usage

```bash
cyphal-reassemble < frames.arrows > transfers.arrows
```

### Input schema (Arrow IPC stream)

| Column | Type | Required |
| --- | --- | --- |
| `timestamp` | `timestamp[us, UTC]` | yes |
| `id` | `uint32` (29-bit extended CAN ID) | yes |
| `data` | `binary` (full CAN payload incl. tail byte) | yes |
| `rti` | `uint8` (redundant interface index) | no (default 0) |

Extra columns are ignored. One invocation handles one reassembly domain
(typically one CAN channel).

### Output schema (Arrow IPC stream)

`timestamp, type, id, source, dest, priority, transfer_id, payload, length`
(matches `sc_schema.TransferSchema` minus `channel`, which the pipeline
re-attaches).

Diagnostics and a summary line (`frames_in / transfers_out / errors / oom`) go
to stderr. Exit code is non-zero only on fatal I/O or schema errors.

## Test

```bash
ctest --test-dir build --output-on-failure
```

Golden fixtures under `tests/data/` are generated with `tools/export_fixtures.py`
from the frame-decoding pipeline.

## License

MIT
````

- [ ] **Step 2: Write the GitHub Actions workflow**

Create `.github/workflows/ci.yml`:
```yaml
name: CI

on:
  push:
    branches: [main, master]
  pull_request:

jobs:
  build-and-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: Install Arrow C++ and build tools
        run: |
          sudo apt-get update
          sudo apt-get install -y -V ca-certificates lsb-release wget cmake g++
          wget https://apache.jfrog.io/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
          sudo apt-get install -y -V ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
          sudo apt-get update
          sudo apt-get install -y -V libarrow-dev

      - name: Configure
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release

      - name: Build
        run: cmake --build build -j

      - name: Test
        run: ctest --test-dir build --output-on-failure
```

Note: the golden tests require the vendored `.arrows` fixtures to be committed
(Task 8). CI does not regenerate them; it consumes what is in `tests/data/`.

- [ ] **Step 3: Verify the workflow file is valid YAML and build still works locally**

Run:
```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```
Expected: full suite passes locally.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "docs: add README and GitHub Actions CI workflow"
```

---

## Self-Review Notes (for the implementer)

- **Spec coverage:** every spec section maps to a task — CAN ID decode (T2), promiscuous multi-instance routing (T3–T4), lazy subscriptions + extent + fixed TID timeout (T3–T4), Arrow IPC stream I/O + schema validation (T5–T6), CLI + stderr summary + exit codes (T7), vendored golden fixtures + tolerant comparison (T8), system-Arrow CMake build + GitHub CI (T1, T9).
- **Type consistency:** output columns and types are identical across T6 (`TransferWriter::schema`) and the spec table (`id: int16`, `source/dest/priority/transfer_id: uint8`, `length: int32`). `CanardTransferKind` string mapping is identical in T4 and matches the pipeline's `"Message"/"Request"/"Response"`.
- **Known tolerated differences:** transfer timestamp (first-frame) and transfer-ID timeout are excluded from the golden comparison key (T8), per the spec's Correctness target.
- **Fixture prerequisite:** T8 Step 2 must be run once (in the pipeline env) before the golden tests can pass; the generated `.arrows` files are committed.
```
