#include "jpeg_utils.h"
#include "binary_io.h"
#include "file_utils.h"

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
    DQT_16BIT_TABLE_SIZE     = 128,
    DEFAULT_QUALITY_ESTIMATE = 80;

constexpr uint16_t TAG_ORIENTATION = 0x0112;

constexpr auto EXIF_SIG = std::to_array<Byte>({'E', 'x', 'i', 'f', '\0', '\0'});
constexpr auto DQT_SIG  = std::to_array<Byte>({0xFF, 0xDB});

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
    int                     width  = 0;
    int                     height = 0;
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

        if (isSofMarker(marker) && payload_size >= 6) {
            out.height = (static_cast<int>(jpg[payload_off + 1]) << 8) | jpg[payload_off + 2];
            out.width  = (static_cast<int>(jpg[payload_off + 3]) << 8) | jpg[payload_off + 4];
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
// (used as the trim offset) and estimate quality from its luminance table.
// Replaces three previously separate scans: estimateImageQuality + two
// caller-side searchSig(DQT1_SIG / DQT2_SIG) passes.
struct DqtFind {
    std::size_t offset  = 0;
    int         quality = DEFAULT_QUALITY_ESTIMATE;
};

[[nodiscard]] DqtFind findDqtAndQuality(std::span<const Byte> jpg) {
    const std::size_t jpg_size = jpg.size();
    const auto dqt_pos_opt = searchSig(jpg, DQT_SIG, DQT_SEARCH_LIMIT);
    if (!dqt_pos_opt) return {};

    const std::size_t pos = *dqt_pos_opt;
    DqtFind out{pos, DEFAULT_QUALITY_ESTIMATE};

    if (!spanHasRange(jpg, pos, 4)) return out;

    const auto end_opt = jpegSegmentEndFromLengthOffset(jpg, pos + 2);
    if (!end_opt) return out;
    const std::size_t end = *end_opt;

    std::size_t tp = pos + 4;
    while (tp < end) {
        const Byte header    = jpg[tp++];
        const Byte precision = (header >> 4) & 0x0F;
        const Byte table_id  = header & 0x0F;

        if (precision > 1) break;

        const std::size_t table_size = (precision == 0)
            ? DQT_8BIT_TABLE_SIZE : DQT_16BIT_TABLE_SIZE;
        if (tp + table_size > end) break;

        if (table_id == 0) {
            int sum = 0;
            if (precision == 0) {
                // C++23: std::ranges::fold_left lowers cleanly to vpsadbw under
                // AVX2; the explicit invariant lets GCC drop bounds checks.
                const bool table_in_range =
                    jpg_size >= DQT_8BIT_TABLE_SIZE && tp <= jpg_size - DQT_8BIT_TABLE_SIZE;
                [[assume(table_in_range)]];
                sum = std::ranges::fold_left(
                    jpg.subspan(tp, DQT_8BIT_TABLE_SIZE),
                    0,
                    [](int acc, Byte b) { return acc + static_cast<int>(b); });
            } else {
                const bool table_in_range =
                    jpg_size >= DQT_16BIT_TABLE_SIZE && tp <= jpg_size - DQT_16BIT_TABLE_SIZE;
                [[assume(table_in_range)]];
                for (std::size_t i = 0; i < 64; ++i) {
                    sum += (static_cast<int>(jpg[tp + i * 2]) << 8) | jpg[tp + i * 2 + 1];
                }
            }
            out.quality = qualityFromLuminanceSum(sum);
            break;
        }
        tp += table_size;
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
    if (input.empty()) {
        throw std::runtime_error("JPG image is empty!");
    }

    // Single header pass replaces tjDecompressHeader3 + exifOrientation,
    // each of which previously walked the input independently.
    const auto prescan = jpegPrescan(input);
    if (!prescan || prescan->width == 0 || prescan->height == 0) {
        throw std::runtime_error("Image Error: Failed to parse JPEG header.");
    }

    constexpr int MIN_DIMENSION = 400;
    if (prescan->width < MIN_DIMENSION || prescan->height < MIN_DIMENSION) {
        throw std::runtime_error(std::format(
            "Image Error: Dimensions {}x{} are too small.\n"
            "For platform compatibility, cover image must be "
            "at least {}px for both width and height.",
            prescan->width, prescan->height, MIN_DIMENSION));
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

    // Single tail pass replaces estimateImageQuality plus two caller-side
    // searchSig passes for DQT1_SIG / DQT2_SIG.
    constexpr int MAX_ALLOWED_QUALITY = 97;
    const std::span<const Byte> result(dst_buf.data, dst_size);
    const DqtFind dqt = findDqtAndQuality(result);

    if (dqt.quality > MAX_ALLOWED_QUALITY) {
        throw std::runtime_error(std::format(
            "Image Error: Estimated quality {} exceeds maximum ({}).\n"
            "For platform compatibility, cover image quality "
            "must be {} or lower.",
            dqt.quality, MAX_ALLOWED_QUALITY, MAX_ALLOWED_QUALITY));
    }
    if (dqt.offset == 0 || dqt.offset >= dst_size) {
        throw std::runtime_error(
            "Image File Error: No DQT segment found (corrupt or unsupported JPG).");
    }

    OptimizedCover out;
    out.data.assign(dst_buf.data + static_cast<std::ptrdiff_t>(dqt.offset), dst_buf.data + dst_size);
    out.estimated_quality = dqt.quality;
    return out;
}
