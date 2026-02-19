#include "jpeg_utils.h"
#include "binary_io.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <stdexcept>

std::optional<uint16_t> exifOrientation(std::span<const Byte> jpg) {
    constexpr std::size_t EXIF_SEARCH_LIMIT = 4096;
    constexpr auto APP1_SIG = std::to_array<Byte>({0xFF, 0xE1});

    const auto app1_post_opt = searchSig(jpg, APP1_SIG, EXIF_SEARCH_LIMIT);
    if (!app1_post_opt) return std::nullopt;

    const std::size_t pos = *app1_post_opt;
    if (pos + 4 > jpg.size()) return std::nullopt;

    const uint16_t segment_length = (static_cast<uint16_t>(jpg[pos + 2]) << 8) | jpg[pos + 3];
    const std::size_t EXIF_END = pos + 2 + segment_length;
    if (EXIF_END > jpg.size()) return std::nullopt;

    const std::span<const Byte> payload(jpg.data() + pos + 4, segment_length - 2);

    constexpr std::size_t EXIF_HEADER_SIZE = 6;
    constexpr auto EXIF_SIG = std::to_array<Byte>({'E', 'x', 'i', 'f', '\0', '\0'});

    if (payload.size() < EXIF_HEADER_SIZE || !std::ranges::equal(payload.first(EXIF_HEADER_SIZE), EXIF_SIG)) {
        return std::nullopt;
    }

    const std::span<const Byte> tiff = payload.subspan(EXIF_HEADER_SIZE);
    if (tiff.size() < 8) return std::nullopt;

    bool is_le;
    if      (tiff[0] == 'I' && tiff[1] == 'I') is_le = true;
    else if (tiff[0] == 'M' && tiff[1] == 'M') is_le = false;
    else return std::nullopt;

    auto read16 = [&](std::size_t offset) -> std::optional<uint16_t> {
        if (offset + 2 > tiff.size()) return std::nullopt;
        uint16_t value;
        std::memcpy(&value, tiff.data() + offset, 2);
        return is_le ? value : std::byteswap(value);
    };

    auto read32 = [&](std::size_t offset) -> std::optional<uint32_t> {
        if (offset + 4 > tiff.size()) return std::nullopt;
        uint32_t value;
        std::memcpy(&value, tiff.data() + offset, 4);
        return is_le ? value : std::byteswap(value);
    };

    const auto magic = read16(2);
    if (!magic || *magic != 0x002A) return std::nullopt;

    const auto ifd_offset_opt = read32(4);
    if (!ifd_offset_opt) return std::nullopt;

    const uint32_t ifd_offset = *ifd_offset_opt;
    if (ifd_offset < 8 || ifd_offset >= tiff.size()) return std::nullopt;

    const auto entry_count_opt = read16(ifd_offset);
    if (!entry_count_opt) return std::nullopt;

    constexpr uint16_t   TAG_ORIENTATION = 0x0112;
    constexpr std::size_t ENTRY_SIZE     = 12;

    std::size_t entry_pos = ifd_offset + 2;

    for (uint16_t i = 0; i < *entry_count_opt; ++i) {
        if (entry_pos + ENTRY_SIZE > tiff.size()) return std::nullopt;

        const auto tag_id = read16(entry_pos);
        if (!tag_id) return std::nullopt;

        if (*tag_id == TAG_ORIENTATION) {
            return read16(entry_pos + 8);
        }
        entry_pos += ENTRY_SIZE;
    }
    return std::nullopt;
}

int getTransformOp(uint16_t orientation) {
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

int estimateImageQuality(std::span<const Byte> jpg) {
    constexpr int DEFAULT_QUALITY_ESTIMATE = 80;

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

    constexpr auto DQT_SIG = std::to_array<Byte>({0xFF, 0xDB});
    constexpr std::size_t DQT_SEARCH_LIMIT = 32768;

    const auto dqt_pos_opt = searchSig(jpg, DQT_SIG, DQT_SEARCH_LIMIT);
    if (!dqt_pos_opt) return DEFAULT_QUALITY_ESTIMATE;

    std::size_t pos = *dqt_pos_opt;
    if (pos + 4 > jpg.size()) return DEFAULT_QUALITY_ESTIMATE;

    const std::size_t
        length = (static_cast<std::size_t>(jpg[pos + 2]) << 8) | jpg[pos + 3],
        end = pos + 2 + length;

    if (end > jpg.size()) return DEFAULT_QUALITY_ESTIMATE;

    pos += 4;

    while (pos < end) {
        const Byte
            header    = jpg[pos++],
            precision = (header >> 4) & 0x0F,
            table_id  = header & 0x0F;

        const std::size_t table_size = (precision == 0) ? 64 : 128;
        if (pos + table_size > end) break;

        if (table_id == 0) {
            int sum = 0;
            for (std::size_t i = 0; i < 64; ++i) {
                sum += (precision == 0)
                    ? jpg[pos + i]
                    : (static_cast<int>(jpg[pos + i * 2]) << 8) | jpg[pos + i * 2 + 1];
            }

            if (sum <= 64)    return 100;
            if (sum >= 16320) return 1;

            for (int q = 1; q <= 100; ++q) {
                if (sum >= STD_LUMINANCE_SUMS[q]) {
                    if (q > 1) {
                        const int
                            diff_current = sum - STD_LUMINANCE_SUMS[q],
                            diff_prev    = STD_LUMINANCE_SUMS[q - 1] - sum;
                        if (diff_prev < diff_current) return q - 1;
                    }
                    return q;
                }
            }
            return 100;
        }
        pos += table_size;
    }
    return DEFAULT_QUALITY_ESTIMATE;
}

void optimizeImage(vBytes& jpg_vec, bool isProgressive) {
    if (jpg_vec.empty()) {
        throw std::runtime_error("JPG image is empty!");
    }

    auto transformer = TJHandle::makeTransformer();
    if (!transformer) {
        throw std::runtime_error("tjInitTransform() failed");
    }

    int width = 0, height = 0, jpegSubsamp = 0, jpegColorspace = 0;
    if (tjDecompressHeader3(transformer.get(), jpg_vec.data(), static_cast<unsigned long>(jpg_vec.size()), &width, &height, &jpegSubsamp, &jpegColorspace) != 0) {
        throw std::runtime_error(std::format("Image Error: {}", tjGetErrorStr2(transformer.get())));
    }

    constexpr int MIN_DIMENSION = 400;
    if (width < MIN_DIMENSION || height < MIN_DIMENSION) {
        throw std::runtime_error(std::format(
            "Image Error: Dimensions {}x{} are too small.\n"
            "For platform compatibility, cover image must be "
            "at least {}px for both width and height.",
            width, height, MIN_DIMENSION));
    }

    const int xop = [&] {
        if (auto ori = exifOrientation(jpg_vec)) return getTransformOp(*ori);
        return static_cast<int>(TJXOP_NONE);
    }();

    tjtransform xform{};
    xform.op      = xop;
    xform.options = TJXOPT_COPYNONE | TJXOPT_TRIM;
    if (isProgressive) {
        xform.options |= TJXOPT_PROGRESSIVE;
    }

    TJBuffer dst_buf;
    unsigned long dst_size = 0;

    if (tjTransform(transformer.get(), jpg_vec.data(), static_cast<unsigned long>(jpg_vec.size()), 1, &dst_buf.data, &dst_size, &xform, 0) != 0) {
        throw std::runtime_error(std::format("tjTransform: {}", tjGetErrorStr2(transformer.get())));
    }

    constexpr int MAX_ALLOWED_QUALITY = 97;
    const std::span<const Byte> result(dst_buf.data, dst_size);
    const int estimated_quality = estimateImageQuality(result);

    if (estimated_quality > MAX_ALLOWED_QUALITY) {
        throw std::runtime_error(std::format(
            "Image Error: Estimated quality {} exceeds maximum ({}).\n"
            "For platform compatibility, cover image quality "
            "must be {} or lower.",
            estimated_quality, MAX_ALLOWED_QUALITY, MAX_ALLOWED_QUALITY));
    }
    jpg_vec.assign(dst_buf.data, dst_buf.data + dst_size);
}
