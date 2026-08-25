#pragma once

#include "common.h"

#include <memory>
#include <span>

// Releases a turbojpeg-allocated buffer (tjFree). Defined in jpeg_utils.cpp so
// <turbojpeg.h> stays out of this header.
struct TjBufferDeleter {
    void operator()(unsigned char* buffer) const noexcept;
};

// Owns the transformed JPEG exactly as turbojpeg produced it. `offset` drops the
// leading markers that conceal replaces with its own segment template, so the
// trim is a view rather than a second full-image allocation and copy.
class OptimizedCover {
public:
    OptimizedCover() = default;

    OptimizedCover(
        std::unique_ptr<unsigned char, TjBufferDeleter> buffer,
        std::size_t offset,
        std::size_t size) noexcept
        : buffer_(std::move(buffer)), offset_(offset), size_(size) {}

    [[nodiscard]] std::span<const Byte> view() const noexcept {
        if (!buffer_) return {};
        return std::span<const Byte>(reinterpret_cast<const Byte*>(buffer_.get()) + offset_, size_);
    }

    // Complete transformed JPEG, including SOI and the leading marker region.
    // Reddit's pixel transcode needs a standalone JPEG rather than the
    // DQT-onward view used by metadata concealment.
    [[nodiscard]] std::span<const Byte> full_view() const noexcept {
        if (!buffer_) return {};
        return std::span<const Byte>(
            reinterpret_cast<const Byte*>(buffer_.get()),
            offset_ + size_);
    }

    [[nodiscard]] std::size_t trimmed_size() const noexcept { return size_; }

private:
    std::unique_ptr<unsigned char, TjBufferDeleter> buffer_{};
    std::size_t offset_{0};
    std::size_t size_{0};
};

[[nodiscard]] OptimizedCover optimizeImage(std::span<const Byte> input, bool isProgressive, bool enforceQualityLimit);
