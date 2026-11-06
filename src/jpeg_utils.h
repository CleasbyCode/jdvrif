#pragma once

#include "common.h"

#include <memory>
#include <span>

// Releases a turbojpeg-allocated buffer (tjFree). Defined in jpeg_utils.cpp so
// <turbojpeg.h> stays out of this header.
struct TjBufferDeleter {
    static void operator()(unsigned char* buffer) noexcept;
};

// Owns the transformed JPEG exactly as turbojpeg produced it. `offset` drops the
// leading markers that conceal replaces with its own segment template, so the
// trim is a view rather than a second full-image allocation and copy.
// What optimizeImage did to the cover, for the conceal progress report.
struct CoverImageInfo {
    int width{0};
    int height{0};
    int quality{0};
    bool orientation_applied{false};
    // Read from the output image's own frame header.
    bool progressive{false};
    // "YCbCr 4:2:0", "Grayscale", ... (after any rotation, which can swap
    // 4:2:2 and 4:4:0).
    std::string_view color_layout{};
};

class OptimizedCover {
public:
    OptimizedCover() = default;

    OptimizedCover(
        std::unique_ptr<unsigned char, TjBufferDeleter> buffer,
        std::size_t offset,
        std::size_t size,
        std::size_t leading_size,
        CoverImageInfo info = {}) noexcept
        : buffer_(std::move(buffer)), offset_(offset), size_(size), leading_size_(leading_size), info_(info) {}

    [[nodiscard]] std::span<const Byte> view() const noexcept {
        if (!buffer_) return {};
        return std::span<const Byte>(reinterpret_cast<const Byte*>(buffer_.get()) + offset_, size_);
    }

    // view() split in two: the kept Adobe APP14 segment (empty when there is
    // none), then everything from the first DQT on. Default mode writes the
    // Adobe segment straight after SOI, ahead of its ICC segments, which is
    // where X-Twitter puts it when it re-serializes the header.
    [[nodiscard]] std::span<const Byte> leading_segments() const noexcept {
        return view().first(leading_size_);
    }
    [[nodiscard]] std::span<const Byte> body_view() const noexcept {
        return view().subspan(leading_size_);
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
    [[nodiscard]] const CoverImageInfo& info() const noexcept { return info_; }

private:
    std::unique_ptr<unsigned char, TjBufferDeleter> buffer_{};
    std::size_t offset_{0};
    std::size_t size_{0};
    std::size_t leading_size_{0};
    CoverImageInfo info_{};
};

[[nodiscard]] OptimizedCover optimizeImage(
    std::span<const Byte> input,
    bool isProgressive,
    bool enforceQualityLimit,
    CoverImageLimits limits = DEFAULT_COVER_IMAGE_LIMITS);

// For the coefficient carriers (-r, -x), which decode the cover themselves and
// carry none of its markers into their output. True when optimizeImage (as
// those carriers call it: baseline, no quality cap) would change nothing they
// read -- no EXIF orientation to apply, so no flip, rotation or MCU trim --
// and every check it makes already passes on the input, so the cover can go
// to the carrier as it is. Any doubt returns false: optimizeImage then runs
// and accepts or reports exactly as it always has.
[[nodiscard]] bool coverNeedsNoNormalization(
    std::span<const Byte> input,
    CoverImageLimits limits = DEFAULT_COVER_IMAGE_LIMITS);
