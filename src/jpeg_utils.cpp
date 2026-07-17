#include "jpeg_utils.h"
#include "file_utils.h"
#include "signal_utils.h"

#include <turbojpeg.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>

namespace {
struct TJHandle {
    TJHandle() = default;

    ~TJHandle() {
        reset();
    }

    TJHandle(const TJHandle&) = delete;
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
    explicit operator bool() const { return handle_ != nullptr; }

private:
    tjhandle handle_ = nullptr;
};

struct TJBuffer {
    unsigned char* data = nullptr;

    TJBuffer() = default;

    ~TJBuffer() {
        if (data) tjFree(data);
    }

    TJBuffer(const TJBuffer&) = delete;
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

constexpr std::size_t
    EXIF_SEARCH_LIMIT        = 4096,
    EXIF_HEADER_SIZE         = 6,
    TIFF_HEADER_SIZE         = 8,
    IFD_ENTRY_SIZE           = 12,
    DQT_SEARCH_LIMIT         = 32768,
    DQT_8BIT_TABLE_SIZE      = 64,
    DQT_16BIT_TABLE_SIZE     = 128;

constexpr int
    MIN_IMAGE_DIMENSION = 400,
    MAX_IMAGE_DIMENSION = 16'384;

constexpr std::uint64_t MAX_IMAGE_PIXELS = 40'000'000;

constexpr uint16_t TAG_ORIENTATION = 0x0112;

constexpr auto EXIF_SIG = std::to_array<Byte>({'E', 'x', 'i', 'f', '\0', '\0'});

[[nodiscard]] constexpr bool markerHasNoLength(Byte marker) noexcept {
    return marker == 0x01 || marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7);
}

[[nodiscard]] constexpr bool isSofMarker(Byte marker) noexcept {
    // SOF0..SOF15 minus DHT(0xC4), JPG(0xC8), DAC(0xCC)
    return marker >= 0xC0 && marker <= 0xCF
        && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
}

[[nodiscard]] std::optional<std::size_t> jpegSegmentEndFromLengthOffset(std::span<const Byte> jpg, std::size_t length_offset) {
    if (!spanHasRange(jpg, length_offset, 2)) return std::nullopt;

    const std::size_t segment_length =
        (static_cast<std::size_t>(jpg[length_offset]) << 8) |
        static_cast<std::size_t>(jpg[length_offset + 1]);
    if (segment_length < 2 || !spanHasRange(jpg, length_offset, segment_length)) {
        return std::nullopt;
    }
    return length_offset + segment_length;
}

constexpr std::array<int, 101> STD_LUMINANCE_SUMS = {
    0,
    16320, 16315, 15946, 15277, 14655, 14073, 13623, 13230, 12859, 12560,
    12240, 11861, 11456, 11081, 10714, 10360, 10027, 9679,  9368,  9056,
    8680,  8331,  7995,  7668,  7376,  7084,  6823,  6562,  6345,  6125,
    5939,  5756,  5571,  5421,  5240,  5086,  4976,  4829,  4719,  4616,
    4463,  4393,  4280,  4166,  4092,  3980,  3909,  3835,  3755,  3688,
    3621,  3541,  3467,  3396,  3323,  3247,  3170,  3096,  3021,  2952,
    2874,  2804,  2727,  2657,  2583,  2509,  2437,  2362,  2290,  2211,
    2136,  2068,  1996,  1915,  1858,  1773,  1692,  1620,  1552,  1477,
    1398,  1326,  1251,  1179,  1109,  1031,  961,   884,   814,   736,
    667,   592,   518,   441,   369,   292,   221,   151,   86,    64
};

struct TiffReader {
    std::span<const Byte> tiff{};
    bool is_little_endian{false};

    [[nodiscard]] std::optional<uint16_t> read16(std::size_t offset) const {
        if (!spanHasRange(tiff, offset, 2)) return std::nullopt;
        uint16_t value;
        std::memcpy(&value, tiff.data() + offset, sizeof(value));
        const bool needs_swap = is_little_endian != (std::endian::native == std::endian::little);
        return needs_swap ? std::byteswap(value) : value;
    }

    [[nodiscard]] std::optional<uint32_t> read32(std::size_t offset) const {
        if (!spanHasRange(tiff, offset, 4)) return std::nullopt;
        uint32_t value;
        std::memcpy(&value, tiff.data() + offset, sizeof(value));
        const bool needs_swap = is_little_endian != (std::endian::native == std::endian::little);
        return needs_swap ? std::byteswap(value) : value;
    }
};

[[nodiscard]] std::optional<TiffReader> makeTiffReader(std::span<const Byte> tiff) {
    if (tiff.size() < TIFF_HEADER_SIZE) return std::nullopt;

    bool is_le;
    if (tiff[0] == 'I' && tiff[1] == 'I') is_le = true;
    else if (tiff[0] == 'M' && tiff[1] == 'M') is_le = false;
    else return std::nullopt;
    return TiffReader{tiff, is_le};
}

[[nodiscard]] std::optional<uint16_t> exifOrientationFromTiff(std::span<const Byte> tiff) {
    const auto reader_opt = makeTiffReader(tiff);
    if (!reader_opt) return std::nullopt;
    const TiffReader reader = *reader_opt;

    const auto magic = reader.read16(2);
    if (!magic || *magic != 0x002A) return std::nullopt;

    const auto ifd_offset_opt = reader.read32(4);
    if (!ifd_offset_opt || !spanHasRange(tiff, *ifd_offset_opt, 2)) return std::nullopt;

    const std::size_t ifd_offset = *ifd_offset_opt;
    const auto entry_count_opt = reader.read16(ifd_offset);
    if (!entry_count_opt) return std::nullopt;

    std::size_t entry_pos = ifd_offset + 2;
    for (uint16_t i = 0; i < *entry_count_opt; ++i) {
        if (!spanHasRange(tiff, entry_pos, IFD_ENTRY_SIZE)) return std::nullopt;

        const auto tag_id = reader.read16(entry_pos);
        if (!tag_id) return std::nullopt;

        if (*tag_id == TAG_ORIENTATION) {
            return reader.read16(entry_pos + 8);
        }
        entry_pos += IFD_ENTRY_SIZE;
    }
    return std::nullopt;
}

[[nodiscard]] int qualityFromLuminanceSum(int sum) {
    if (sum <= 64) return 100;
    if (sum >= 16320) return 1;

    for (std::size_t q = 1; q < STD_LUMINANCE_SUMS.size(); ++q) {
        if (sum >= STD_LUMINANCE_SUMS[q]) {
            if (q > 1) {
                const int diff_current = sum - STD_LUMINANCE_SUMS[q];
                const int diff_prev = STD_LUMINANCE_SUMS[q - 1] - sum;
                if (diff_prev < diff_current) return static_cast<int>(q - 1);
            }
            return static_cast<int>(q);
        }
    }
    return 100;
}

// Single-pass prescan of the *input* JPEG: extract SOF dimensions and EXIF
// orientation in one marker walk. Replaces tjDecompressHeader3 plus the
// separate exifPayload/exifOrientation passes (each of which previously
// re-walked the same bytes).
struct JpegPrescan {
    int                     width      = 0;
    int                     height     = 0;
    int                     components = 0;
    std::optional<uint16_t> orientation;
};

[[nodiscard]] std::optional<JpegPrescan> jpegPrescan(std::span<const Byte> jpg) {
    if (jpg.size() < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) return std::nullopt;

    JpegPrescan out;
    std::size_t pos = 2;
    const std::size_t exif_limit = std::min(jpg.size(), EXIF_SEARCH_LIMIT);

    while (pos + 4 <= jpg.size()) {
        while (pos < jpg.size() && jpg[pos] == 0xFF) ++pos;
        if (pos >= jpg.size()) break;

        const Byte marker = jpg[pos++];
        if (marker == 0x00 || markerHasNoLength(marker)) continue;
        if (marker == 0xDA) break;  // SOS: scan data follows, stop walking.

        const auto end_opt = jpegSegmentEndFromLengthOffset(jpg, pos);
        if (!end_opt) return std::nullopt;
        const std::size_t payload_off  = pos + 2;
        const std::size_t payload_size = *end_opt - payload_off;

        if (isSofMarker(marker) && payload_size >= 6 && out.width == 0) {
            const int components = static_cast<int>(jpg[payload_off + 5]);
            const std::size_t component_bytes = static_cast<std::size_t>(components) * 3;
            if (components == 0 || component_bytes > payload_size - 6) {
                return std::nullopt;
            }
            out.height = (static_cast<int>(jpg[payload_off + 1]) << 8) | jpg[payload_off + 2];
            out.width  = (static_cast<int>(jpg[payload_off + 3]) << 8) | jpg[payload_off + 4];
            out.components = components;
        } else if (marker == 0xE1 && pos <= exif_limit &&
                   payload_size >= EXIF_HEADER_SIZE &&
                   std::ranges::equal(jpg.subspan(payload_off, EXIF_HEADER_SIZE), EXIF_SIG)) {
            out.orientation = exifOrientationFromTiff(
                jpg.subspan(payload_off + EXIF_HEADER_SIZE,
                            payload_size - EXIF_HEADER_SIZE));
        }

        if (out.width != 0 && out.orientation.has_value()) break;

        pos = *end_opt;
    }
    return out;
}

// Single-pass scan of the *transformed* JPEG: locate the first DQT marker
// (used as the trim offset), read the transformed dimensions, and estimate
// quality from the quantization table actually selected by the first SOF
// component. JPEG quantization table IDs are not required to start at zero.
struct TransformedJpegInfo {
    std::size_t offset  = 0;
    int         width   = 0;
    int         height  = 0;
    std::optional<int> quality;
};

[[nodiscard]] TransformedJpegInfo inspectTransformedJpeg(std::span<const Byte> jpg) {
    TransformedJpegInfo out;
    if (jpg.size() < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) return out;

    std::array<std::optional<int>, 4> table_qualities{};
    std::optional<Byte> primary_table_id;

    std::size_t pos = 2;
    while (pos + 2 <= jpg.size()) {
        while (pos < jpg.size() && jpg[pos] == 0xFF) ++pos;
        if (pos >= jpg.size()) break;

        const Byte marker = jpg[pos++];
        const std::size_t marker_offset = pos - 2;
        if (marker == 0x00 || markerHasNoLength(marker)) continue;
        if (marker == 0xDA) break;

        const auto end_opt = jpegSegmentEndFromLengthOffset(jpg, pos);
        if (!end_opt) return {};
        const std::size_t payload_off  = pos + 2;
        const std::size_t payload_size = *end_opt - payload_off;

        if (marker == 0xDB) {
            if (out.offset == 0 &&
                marker_offset <= DQT_SEARCH_LIMIT - 2) {
                out.offset = marker_offset;
            }

            std::size_t tp = payload_off;
            while (tp < *end_opt) {
                const Byte header    = jpg[tp++];
                const Byte precision = (header >> 4) & 0x0F;
                const Byte table_id  = header & 0x0F;
                if (precision > 1 || table_id >= table_qualities.size()) return {};

                const std::size_t table_size = (precision == 0)
                    ? DQT_8BIT_TABLE_SIZE : DQT_16BIT_TABLE_SIZE;
                if (!spanHasRange(jpg, tp, table_size) || tp + table_size > *end_opt) {
                    return {};
                }

                int sum = 0;
                if (precision == 0) {
                    // C++23: std::ranges::fold_left lowers cleanly to vpsadbw under
                    // AVX2; the explicit invariant lets GCC drop bounds checks.
                    const bool table_in_range =
                        jpg.size() >= DQT_8BIT_TABLE_SIZE &&
                        tp <= jpg.size() - DQT_8BIT_TABLE_SIZE;
                    [[assume(table_in_range)]];
                    sum = std::ranges::fold_left(
                        jpg.subspan(tp, DQT_8BIT_TABLE_SIZE),
                        0,
                        [](int acc, Byte b) { return acc + static_cast<int>(b); });
                } else {
                    const bool table_in_range =
                        jpg.size() >= DQT_16BIT_TABLE_SIZE &&
                        tp <= jpg.size() - DQT_16BIT_TABLE_SIZE;
                    [[assume(table_in_range)]];
                    for (std::size_t i = 0; i < 64; ++i) {
                        sum += (static_cast<int>(jpg[tp + i * 2]) << 8) |
                               jpg[tp + i * 2 + 1];
                    }
                }
                table_qualities[table_id] = qualityFromLuminanceSum(sum);
                tp += table_size;
            }
        } else if (isSofMarker(marker)) {
            if (payload_size < 9) return {};
            const std::size_t components = jpg[payload_off + 5];
            if (components == 0 || components > (payload_size - 6) / 3) return {};

            out.height = (static_cast<int>(jpg[payload_off + 1]) << 8) |
                         jpg[payload_off + 2];
            out.width  = (static_cast<int>(jpg[payload_off + 3]) << 8) |
                         jpg[payload_off + 4];
            primary_table_id = jpg[payload_off + 8];
        }

        pos = *end_opt;
    }

    if (primary_table_id && *primary_table_id < table_qualities.size()) {
        out.quality = table_qualities[*primary_table_id];
    }
    return out;
}

[[nodiscard]] int getTransformOp(uint16_t orientation) {
    switch (orientation) {
        case 2: return TJXOP_HFLIP;
        case 3: return TJXOP_ROT180;
        case 4: return TJXOP_VFLIP;
        case 5: return TJXOP_TRANSPOSE;
        case 6: return TJXOP_ROT90;
        case 7: return TJXOP_TRANSVERSE;
        case 8: return TJXOP_ROT270;
        default: return TJXOP_NONE;
    }
}
} // namespace

OptimizedCover optimizeImage(std::span<const Byte> input, bool isProgressive) {
    throwIfSignalCancellationRequested();
    if (input.empty()) {
        throw std::runtime_error("JPG image is empty!");
    }

    // Single header pass replaces tjDecompressHeader3 + exifOrientation,
    // each of which previously walked the input independently.
    const auto prescan = jpegPrescan(input);
    if (!prescan || prescan->width == 0 || prescan->height == 0) {
        throw std::runtime_error("Image Error: Failed to parse JPEG header.");
    }

    if (prescan->width < MIN_IMAGE_DIMENSION || prescan->height < MIN_IMAGE_DIMENSION) {
        throw std::runtime_error(std::format(
            "Image Error: Dimensions {}x{} are too small.\n"
            "For platform compatibility, cover image must be "
            "at least {}px for both width and height.",
            prescan->width, prescan->height, MIN_IMAGE_DIMENSION));
    }

    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(prescan->width) *
        static_cast<std::uint64_t>(prescan->height);
    if (prescan->width > MAX_IMAGE_DIMENSION ||
        prescan->height > MAX_IMAGE_DIMENSION ||
        pixel_count > MAX_IMAGE_PIXELS) {
        throw std::runtime_error(std::format(
            "Image Error: Dimensions {}x{} exceed the safe cover-image limit "
            "({}px per dimension and {} total pixels).",
            prescan->width, prescan->height,
            MAX_IMAGE_DIMENSION, MAX_IMAGE_PIXELS));
    }

    if (prescan->components != 1 && prescan->components != 3) {
        throw std::runtime_error(std::format(
            "Image Error: Unsupported JPEG color space ({} components). "
            "CMYK/YCCK cover images must be converted to RGB before use.",
            prescan->components));
    }

    auto transformer = TJHandle::makeTransformer();
    if (!transformer) {
        throw std::runtime_error("tjInitTransform() failed");
    }

    const int xop = prescan->orientation
        ? getTransformOp(*prescan->orientation)
        : static_cast<int>(TJXOP_NONE);

    tjtransform xform{};
    xform.op      = xop;
    xform.options = TJXOPT_COPYNONE | TJXOPT_TRIM;
    if (isProgressive) xform.options |= TJXOPT_PROGRESSIVE;

    TJBuffer dst_buf;
    unsigned long dst_size = 0;

    if (tjTransform(
            transformer.get(),
            input.data(),
            static_cast<unsigned long>(input.size()),
            1,
            &dst_buf.data,
            &dst_size,
            &xform,
            0) != 0) {
        throw std::runtime_error(std::format("tjTransform: {}", tjGetErrorStr2(transformer.get())));
    }
    throwIfSignalCancellationRequested();

    // Single tail pass replaces estimateImageQuality plus two caller-side
    // searchSig passes for DQT1_SIG / DQT2_SIG.
    constexpr int MAX_ALLOWED_QUALITY = 97;
    const std::span<const Byte> result(dst_buf.data, dst_size);
    const TransformedJpegInfo transformed = inspectTransformedJpeg(result);

    if (transformed.width == 0 || transformed.height == 0) {
        throw std::runtime_error(
            "Image Error: Failed to parse transformed JPEG header.");
    }
    if (transformed.width < MIN_IMAGE_DIMENSION ||
        transformed.height < MIN_IMAGE_DIMENSION) {
        throw std::runtime_error(std::format(
            "Image Error: Lossless orientation/MCU trimming reduced dimensions "
            "to {}x{}. Cover image must remain at least {}px for both width and height.",
            transformed.width, transformed.height, MIN_IMAGE_DIMENSION));
    }

    if (!transformed.quality) {
        throw std::runtime_error(
            "Image File Error: Quantization table referenced by JPEG components "
            "was not found (corrupt or unsupported JPG).");
    }
    if (*transformed.quality > MAX_ALLOWED_QUALITY) {
        throw std::runtime_error(std::format(
            "Image Error: Estimated quality {} exceeds maximum ({}).\n"
            "For platform compatibility, cover image quality "
            "must be {} or lower.",
            *transformed.quality, MAX_ALLOWED_QUALITY, MAX_ALLOWED_QUALITY));
    }
    if (transformed.offset == 0 || transformed.offset >= dst_size) {
        throw std::runtime_error(
            "Image File Error: No DQT segment found (corrupt or unsupported JPG).");
    }

    OptimizedCover out;
    out.data.assign(
        dst_buf.data + static_cast<std::ptrdiff_t>(transformed.offset),
        dst_buf.data + dst_size);
    return out;
}
