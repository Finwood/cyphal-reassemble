#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

// Classic frames and short CAN FD payloads stay inline so a frame does not allocate.
struct Payload {
    static constexpr size_t kInline = 16;

    Payload() = default;

    Payload(const Payload& other) { copy_from(other); }
    Payload& operator=(const Payload& other) {
        if (this != &other) copy_from(other);
        return *this;
    }
    Payload(Payload&& other) noexcept { take(std::move(other)); }
    Payload& operator=(Payload&& other) noexcept {
        if (this != &other) take(std::move(other));
        return *this;
    }
    Payload& operator=(std::initializer_list<uint8_t> bytes) {
        assign(bytes.begin(), bytes.size());
        return *this;
    }

    void assign(const uint8_t* bytes, size_t n) {
        if (n > kInline) {
            auto fresh = std::make_unique<uint8_t[]>(n);
            if (n != 0 && bytes != nullptr) std::memcpy(fresh.get(), bytes, n);
            heap_ = std::move(fresh);
        } else {
            heap_.reset();
            if (n != 0 && bytes != nullptr) std::memcpy(inline_, bytes, n);
        }
        size_ = n;
    }

    const uint8_t* data() const { return heap_ ? heap_.get() : inline_; }
    size_t size() const { return size_; }

    friend bool operator==(const Payload& a, const Payload& b) {
        return a.size_ == b.size_ && (a.size_ == 0 || std::memcmp(a.data(), b.data(), a.size_) == 0);
    }
    friend bool operator!=(const Payload& a, const Payload& b) { return !(a == b); }
    friend bool operator<(const Payload& a, const Payload& b) {
        const size_t n = a.size_ < b.size_ ? a.size_ : b.size_;
        if (n != 0) {
            const int cmp = std::memcmp(a.data(), b.data(), n);
            if (cmp != 0) return cmp < 0;
        }
        return a.size_ < b.size_;
    }

private:
    void copy_from(const Payload& other) { assign(other.data(), other.size_); }
    void take(Payload&& other) noexcept {
        size_ = other.size_;
        if (other.heap_) {
            heap_ = std::move(other.heap_);
        } else {
            heap_.reset();
            if (size_ != 0) std::memcpy(inline_, other.inline_, size_);
        }
        other.size_ = 0;
    }

    uint8_t inline_[kInline]{};
    std::unique_ptr<uint8_t[]> heap_;
    size_t size_ = 0;
};

inline bool operator==(const Payload& a, const std::vector<uint8_t>& b) {
    return a.size() == b.size() && (a.size() == 0 || std::memcmp(a.data(), b.data(), a.size()) == 0);
}
inline bool operator==(const std::vector<uint8_t>& a, const Payload& b) { return b == a; }
inline bool operator!=(const Payload& a, const std::vector<uint8_t>& b) { return !(a == b); }
inline bool operator!=(const std::vector<uint8_t>& a, const Payload& b) { return !(b == a); }

struct Frame {
    std::string channel;
    int64_t timestamp_us = 0;
    uint32_t id = 0;
    uint8_t dlc = 0;
    bool ide = false;
    bool rtr = false;
    bool err = false;
    bool edl = false;
    bool brs = false;
    bool esi = false;
    bool tx = false;
    Payload data;
};

struct DecodeResult {
    std::string error;
    std::vector<Frame> frames;
};

DecodeResult decode_mf4(const uint8_t* data, size_t size);
