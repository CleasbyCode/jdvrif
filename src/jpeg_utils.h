#pragma once

#include "common.h"

#include <optional>
#include <span>

/// RAII wrapper for a TurboJPEG handle (decompressor, compressor, or transformer).
struct TJHandle {
    TJHandle() = default;
    ~TJHandle() { reset(); }

    TJHandle(const TJHandle&)            = delete;
    TJHandle& operator=(const TJHandle&) = delete;

    TJHandle(TJHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    TJHandle& operator=(TJHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] static TJHandle makeTransformer() {
        TJHandle h;
        h.handle_ = tjInitTransform();
        return h;
    }

    void reset() {
        if (handle_) {
            tjDestroy(handle_);
            handle_ = nullptr;
        }
    }

    [[nodiscard]] tjhandle get() const { return handle_; }
    explicit operator bool() const     { return handle_ != nullptr; }

private:
    tjhandle handle_ = nullptr;
};

/// RAII wrapper for a TurboJPEG-allocated output buffer.
struct TJBuffer {
    unsigned char* data = nullptr;

    TJBuffer() = default;
    ~TJBuffer() { if (data) tjFree(data); }

    TJBuffer(const TJBuffer&)            = delete;
    TJBuffer& operator=(const TJBuffer&) = delete;

    TJBuffer(TJBuffer&& other) noexcept : data(other.data) {
        other.data = nullptr;
    }

    TJBuffer& operator=(TJBuffer&& other) noexcept {
        if (this != &other) {
            if (data) tjFree(data);
            data = other.data;
            other.data = nullptr;
        }
        return *this;
    }
};

[[nodiscard]] std::optional<uint16_t> exifOrientation(std::span<const Byte> jpg);
[[nodiscard]] int getTransformOp(uint16_t orientation);
[[nodiscard]] int estimateImageQuality(std::span<const Byte> jpg);
void optimizeImage(vBytes& jpg_vec, bool isProgressive);
