#include "jpeg_utils.h"
#include "carrier_common.h"
#include "file_utils.h"
#include "jpeg_safety.h"
#include "signal_utils.h"

#include <turbojpeg.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>

void TjBufferDeleter::operator()(unsigned char* buffer) noexcept {
    if (buffer) tjFree(buffer);
}

namespace {
// Owns a TurboJPEG handle. tjhandle is a void*, so the unique_ptr holds void;
// callers use only get() and the null test.
struct TjHandleDeleter {
    void operator()(tjhandle handle) const noexcept { tjDestroy(handle); }
};
using TjHandle = std::unique_ptr<std::remove_pointer_t<tjhandle>, TjHandleDeleter>;

// Null when TurboJPEG cannot allocate a transformer; callers check.
[[nodiscard]] TjHandle makeTransformer() {
    return TjHandle(tjInitTransform());
}

constexpr std::size_t
    EXIF_HEADER_SIZE         = 6,
    TIFF_HEADER_SIZE         = 8,
    IFD_ENTRY_SIZE           = 12,
    DQT_SEARCH_LIMIT         = 32768,
    DQT_8BIT_TABLE_SIZE      = 64,
    DQT_16BIT_TABLE_SIZE     = 128;

constexpr int MIN_IMAGE_DIMENSION = 400;

constexpr uint16_t TAG_ORIENTATION = 0x0112;
constexpr uint16_t TIFF_TYPE_SHORT = 3;

constexpr auto EXIF_SIG = std::to_array<Byte>({'E', 'x', 'i', 'f', '\0', '\0'});

// TEM and RST0..7: no length field, and libjpeg skips them between header
// segments. SOI and EOI are not here: in the header, a second SOI or an EOI
// before any scan makes libjpeg refuse the file, so the walkers do too.
[[nodiscard]] constexpr bool markerHasNoLength(Byte marker) noexcept {
    return marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7);
}

[[nodiscard]] constexpr bool isSoiOrEoi(Byte marker) noexcept {
    return marker == 0xD8 || marker == 0xD9;
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
    if (segment_length < 2 || !spanHasRange(jpg, length_offset, segment_length)) return std::nullopt;
    return length_offset + segment_length;
}

struct TiffReader {
    std::span<const Byte> tiff{};
    bool is_little_endian{false};

    // TIFF fields are stored in the byte order the header declares, so swap only
    // when that disagrees with the host.
    template<typename T>
    [[nodiscard]] std::optional<T> read(std::size_t offset) const {
        if (!spanHasRange(tiff, offset, sizeof(T))) return std::nullopt;
        T value;
        std::memcpy(&value, tiff.data() + offset, sizeof(value));
        const bool needs_swap = is_little_endian != (std::endian::native == std::endian::little);
        return needs_swap ? std::byteswap(value) : value;
    }

    [[nodiscard]] std::optional<uint16_t> read16(std::size_t offset) const { return read<uint16_t>(offset); }
    [[nodiscard]] std::optional<uint32_t> read32(std::size_t offset) const { return read<uint32_t>(offset); }
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
            // EXIF defines Orientation as one SHORT, and browsers ignore the tag
            // stored any other way, so a cover must not be rotated for one.
            const auto type = reader.read16(entry_pos + 2);
            const auto count = reader.read32(entry_pos + 4);
            if (type != TIFF_TYPE_SHORT || count != 1u) return std::nullopt;
            return reader.read16(entry_pos + 8);
        }
        entry_pos += IFD_ENTRY_SIZE;
    }
    return std::nullopt;
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

// The marker-segment walk both header readers share, from just past SOI up to
// SOS. Each segment must start with 0xFF (fill bytes allowed): a bare byte is
// not a marker to libjpeg, which skips it as junk, so trusting it here would
// let a fake SOF supply the dimensions checked against the size limits while
// the decoder uses the real one. FF00 (junk libjpeg discards) and the
// standalone markers are skipped; SOI or EOI before SOS refuses the image.
// visit(marker, marker_offset, payload) sees every other segment and returns
// false to refuse it. True only once SOS is reached.
template<typename Visit>
[[nodiscard]] bool walkHeaderSegments(std::span<const Byte> jpg, Visit&& visit) {
    if (jpg.size() < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) return false;

    std::size_t pos = 2;
    while (pos + 2 <= jpg.size()) {
        if (jpg[pos] != 0xFF) return false;
        while (pos < jpg.size() && jpg[pos] == 0xFF) ++pos;
        if (pos >= jpg.size()) return false;

        const Byte marker = jpg[pos++];
        const std::size_t marker_offset = pos - 2;
        if (isSoiOrEoi(marker)) return false;
        if (marker == 0x00 || markerHasNoLength(marker)) continue;
        if (marker == 0xDA) return true;  // SOS: scan data follows.

        const auto end_opt = jpegSegmentEndFromLengthOffset(jpg, pos);
        if (!end_opt) return false;
        const std::size_t payload_off = pos + 2;
        if (!visit(marker, marker_offset, jpg.subspan(payload_off, *end_opt - payload_off))) return false;
        pos = *end_opt;
    }
    return false;
}

// SOF dimensions and the EXIF orientation of the *input* JPEG, read from the
// first EXIF APP1 wherever it sits (large COM/APPn segments may come first).
[[nodiscard]] std::optional<JpegPrescan> jpegPrescan(std::span<const Byte> jpg) {
    JpegPrescan out;
    bool saw_sof = false;
    const bool complete = walkHeaderSegments(jpg, [&](Byte marker, std::size_t, std::span<const Byte> payload) {
        if (isSofMarker(marker)) {
            // A second frame header is either hierarchical JPEG (unsupported)
            // or an attempt to make this scan and the decoder disagree.
            if (saw_sof || payload.size() < 6) return false;
            saw_sof = true;
            const int components = static_cast<int>(payload[5]);
            const std::size_t component_bytes = static_cast<std::size_t>(components) * 3;
            if (components == 0 || component_bytes > payload.size() - 6) return false;
            out.height = (static_cast<int>(payload[1]) << 8) | payload[2];
            out.width  = (static_cast<int>(payload[3]) << 8) | payload[4];
            out.components = components;
        } else if (marker == 0xE1 && !out.orientation &&
                   payload.size() >= EXIF_HEADER_SIZE &&
                   std::ranges::equal(payload.first(EXIF_HEADER_SIZE), EXIF_SIG)) {
            out.orientation = exifOrientationFromTiff(payload.subspan(EXIF_HEADER_SIZE));
        }
        return true;
    });
    if (!complete) return std::nullopt;
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
    bool        progressive = false;
    std::optional<int> quality;
};

// Colour space plus chroma subsampling, from TurboJPEG's header values.
[[nodiscard]] std::string_view colorLayoutName(int subsamp, int colorspace) {
    switch (colorspace) {
        case TJCS_GRAY: return "Grayscale";
        case TJCS_RGB:  return "RGB";
        case TJCS_CMYK: return "CMYK";
        default: break;
    }
    const bool ycck = colorspace == TJCS_YCCK;
    switch (subsamp) {
        case TJSAMP_444: return ycck ? "YCCK 4:4:4" : "YCbCr 4:4:4";
        case TJSAMP_422: return ycck ? "YCCK 4:2:2" : "YCbCr 4:2:2";
        case TJSAMP_420: return ycck ? "YCCK 4:2:0" : "YCbCr 4:2:0";
        case TJSAMP_440: return ycck ? "YCCK 4:4:0" : "YCbCr 4:4:0";
        case TJSAMP_411: return ycck ? "YCCK 4:1:1" : "YCbCr 4:1:1";
        case TJSAMP_GRAY: return "Grayscale";
        default: return ycck ? "YCCK (non-standard subsampling)" : "YCbCr (non-standard subsampling)";
    }
}

// libjpeg writes an Adobe APP14 segment for RGB output, and its transform flag
// is what tells a decoder the coefficients are RGB: without it, component IDs
// 1,2,3 are read as YCbCr and the colours come out wrong. The trim to the first
// DQT would drop it, so it is moved to sit just before the DQT (whole segments
// are reordered, so the full JPEG stays valid) and the trim keeps it. Returns
// the new trim offset; anything unexpected leaves the layout untouched.
[[nodiscard]] std::size_t keepAdobeSegment(std::span<Byte> jpg, std::size_t dqt_offset) {
    constexpr std::array<Byte, 5> ADOBE_SIG{'A', 'd', 'o', 'b', 'e'};
    if (dqt_offset > jpg.size()) return dqt_offset;
    std::size_t pos = 2;
    while (pos + 4 <= dqt_offset) {
        if (jpg[pos] != 0xFF) return dqt_offset;
        // Fill bytes may precede any marker; the segment starts at the last.
        if (jpg[pos + 1] == 0xFF) { ++pos; continue; }
        const Byte marker = jpg[pos + 1];
        if (markerHasNoLength(marker)) { pos += 2; continue; }
        const std::size_t segment_size = 2 + ((static_cast<std::size_t>(jpg[pos + 2]) << 8) | jpg[pos + 3]);
        if (segment_size < 4 || segment_size > dqt_offset - pos) return dqt_offset;
        if (marker == 0xEE && segment_size >= 4 + ADOBE_SIG.size() &&
            std::ranges::equal(jpg.subspan(pos + 4, ADOBE_SIG.size()), ADOBE_SIG)) {
            const auto first = jpg.begin() + static_cast<std::ptrdiff_t>(pos);
            std::rotate(first,
                first + static_cast<std::ptrdiff_t>(segment_size),
                jpg.begin() + static_cast<std::ptrdiff_t>(dqt_offset));
            return dqt_offset - segment_size;
        }
        pos += segment_size;
    }
    return dqt_offset;
}

[[nodiscard]] TransformedJpegInfo inspectTransformedJpeg(std::span<const Byte> jpg) {
    TransformedJpegInfo out;
    std::array<std::optional<int>, 4> table_qualities{};
    std::optional<Byte> primary_table_id;

    const bool complete = walkHeaderSegments(jpg, [&](Byte marker, std::size_t marker_offset, std::span<const Byte> payload) {
        if (marker == 0xDB) {
            if (out.offset == 0 && marker_offset <= DQT_SEARCH_LIMIT - 2) {
                out.offset = marker_offset;
            }
            std::size_t tp = 0;
            while (tp < payload.size()) {
                const Byte header    = payload[tp++];
                const Byte precision = (header >> 4) & 0x0F;
                const Byte table_id  = header & 0x0F;
                if (precision > 1 || table_id >= table_qualities.size()) return false;

                const std::size_t table_size = (precision == 0)
                    ? DQT_8BIT_TABLE_SIZE : DQT_16BIT_TABLE_SIZE;
                if (table_size > payload.size() - tp) return false;
                const std::span<const Byte> table = payload.subspan(tp, table_size);

                int sum = 0;
                if (precision == 0) {
                    sum = std::ranges::fold_left(table, 0,
                        [](int acc, Byte b) static { return acc + static_cast<int>(b); });
                } else {
                    for (std::size_t i = 0; i < 64; ++i) {
                        sum += (static_cast<int>(table[i * 2]) << 8) | table[i * 2 + 1];
                    }
                }
                // Same estimator the X-Twitter carrier uses, so the two can never
                // disagree about whether a table is above Q97.
                table_qualities[table_id] =
                    carrier_common::estimateQualityFromLumaSum(static_cast<std::uint64_t>(sum));
                tp += table_size;
            }
        } else if (isSofMarker(marker)) {
            if (payload.size() < 9) return false;
            const std::size_t components = payload[5];
            if (components == 0 || components > (payload.size() - 6) / 3) return false;

            out.height = (static_cast<int>(payload[1]) << 8) | payload[2];
            out.width  = (static_cast<int>(payload[3]) << 8) | payload[4];
            primary_table_id = payload[8];
            // SOF2/6/10/14 are the progressive frame types.
            out.progressive = marker == 0xC2 || marker == 0xC6 || marker == 0xCA || marker == 0xCE;
        }
        return true;
    });
    if (!complete) return {};

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

// The scans libjpeg would decode: SOS markers up to EOI. Marker segments are
// skipped by their length, so FF DA bytes inside an APPn or COM payload (an
// EXIF thumbnail, say) are not counted. Entropy-coded data is walked a byte at
// a time: an FF there is followed by 00 or a restart code, and any other
// marker byte ends the scan for libjpeg too. The bitwise twin of the count
// jpeg_safety::ProgressMonitor makes, available before a decode starts.
[[nodiscard]] std::size_t countScans(std::span<const Byte> jpg) noexcept {
    std::size_t count = 0;
    std::size_t pos = 2;  // past SOI
    while (pos + 1 < jpg.size()) {
        if (jpg[pos] != 0xFF) {
            ++pos;
            continue;
        }
        const Byte marker = jpg[pos + 1];
        if (marker == 0xFF) {  // fill byte
            ++pos;
            continue;
        }
        if (marker == 0x00 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            pos += 2;  // stuffed FF, TEM, RSTn, SOI: no length field
            continue;
        }
        if (marker == 0xD9) break;  // EOI
        if (pos + 3 >= jpg.size()) break;
        const std::size_t length = (static_cast<std::size_t>(jpg[pos + 2]) << 8) | jpg[pos + 3];
        if (length < 2) break;  // libjpeg refuses the image here
        if (marker == 0xDA) ++count;
        pos += 2 + length;  // a scan's entropy-coded data follows its header
    }
    return count;
}

// The scan limit the carriers' own decoders enforce, applied before the one
// decode jdvrif cannot monitor. TurboJPEG's own limit (TJFLAG_LIMITSCANS) is
// 500 scans, and its progress monitor ignores Ctrl+C.
void requireScanLimit(std::span<const Byte> jpg) {
    if (countScans(jpg) > static_cast<std::size_t>(jpeg_safety::MAX_INPUT_SCANS)) {
        throw std::runtime_error(std::format(
            "JPEG error: Progressive scan limit exceeded (maximum {} scans).", jpeg_safety::MAX_INPUT_SCANS));
    }
}

// A cover that passed every check made before its transform: the header
// read, and the TurboJPEG handle that confirmed it.
struct ValidatedCover {
    JpegPrescan prescan;
    TjHandle transformer;
};

// The checks optimizeImage makes before its transform, in its order, with its
// errors: a parseable header, dimensions and components within the limits, at
// most MAX_INPUT_SCANS scans, and the frame libjpeg will actually decode.
[[nodiscard]] ValidatedCover validateCoverInput(std::span<const Byte> input, CoverImageLimits limits) {
    throwIfSignalCancellationRequested();
    throwIf(input.empty(), "JPG image is empty!");

    const auto prescan = jpegPrescan(input);
    throwIf(!prescan || prescan->width == 0 || prescan->height == 0, "Image Error: Failed to parse JPEG header.");

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
    if (static_cast<std::uint64_t>(prescan->width) > limits.max_dimension ||
        static_cast<std::uint64_t>(prescan->height) > limits.max_dimension ||
        pixel_count > limits.max_pixels) {
        throw std::runtime_error(std::format(
            "Image Error: Dimensions {}x{} exceed the safe cover-image limit "
            "({}px per dimension and {} total pixels).",
            prescan->width, prescan->height,
            limits.max_dimension, limits.max_pixels));
    }

    if (prescan->components != 1 && prescan->components != 3) {
        throw std::runtime_error(std::format(
            "Image Error: Unsupported JPEG color space ({} components). "
            "CMYK/YCCK cover images must be converted to RGB before use.",
            prescan->components));
    }

    requireScanLimit(input);

    auto transformer = makeTransformer();
    throwIf(!transformer, "tjInitTransform() failed");

    // Confirm the limits above were checked against the frame libjpeg will
    // actually decode. Header parsing allocates nothing image-sized, so this
    // runs before tjTransform commits to the full decode.
    int header_width = 0, header_height = 0, subsamp = 0, colorspace = 0;
    const int header_rc = tjDecompressHeader3(
        transformer.get(),
        input.data(),
        static_cast<unsigned long>(input.size()),
        &header_width, &header_height, &subsamp, &colorspace);
    throwIf(
        header_rc != 0 ||
            header_width != prescan->width ||
            header_height != prescan->height,
        "Image Error: Failed to parse JPEG header.");

    return ValidatedCover{.prescan = *prescan, .transformer = std::move(transformer)};
}

[[nodiscard]] int transformOp(const JpegPrescan& prescan) {
    return prescan.orientation
        .transform(getTransformOp)
        .value_or(static_cast<int>(TJXOP_NONE));
}

// The lossless transform and the checks on its output.
[[nodiscard]] OptimizedCover transformCover(
    std::span<const Byte> input,
    const ValidatedCover& validated,
    bool isProgressive,
    bool enforceQualityLimit) {
    const TjHandle& transformer = validated.transformer;
    const int xop = transformOp(validated.prescan);

    tjtransform xform{};
    xform.op      = xop;
    xform.options = TJXOPT_COPYNONE | TJXOPT_TRIM;
    if (isProgressive) xform.options |= TJXOPT_PROGRESSIVE;

    std::unique_ptr<unsigned char, TjBufferDeleter> dst_buf;
    unsigned long dst_size = 0;

    // TurboJPEG owns its libjpeg progress monitor internally, so Ctrl+C is
    // noticed only once this call returns. requireScanLimit has already held
    // the input to MAX_INPUT_SCANS, which bounds that wait: the worst cover
    // within the limits (Reddit's 8192x8192, 4:4:4, 100 scans, rotated) runs
    // about 2 s here. TJFLAG_STOPONWARNING stops at the first corrupt-data
    // warning instead of decoding on to a failure reported at the end.
    const int transform_rc = tjTransform(
        transformer.get(),
        input.data(),
        static_cast<unsigned long>(input.size()),
        1,
        // C++23: std::out_ptr adopts the buffer as the call returns, before the
        // result is checked: a failed transform can still have allocated one,
        // and this frees it on every exit path from here on.
        std::out_ptr(dst_buf),
        &dst_size,
        &xform,
        TJFLAG_LIMITSCANS | TJFLAG_STOPONWARNING);

    if (transform_rc != 0) {
        throw std::runtime_error(std::format("tjTransform: {}", tjGetErrorStr2(transformer.get())));
    }
    throwIfSignalCancellationRequested();

    // Single tail pass replaces estimateImageQuality plus two caller-side
    // searchSig passes for DQT1_SIG / DQT2_SIG.
    constexpr int MAX_ALLOWED_QUALITY = 97;
    const std::span<const Byte> result(
        reinterpret_cast<const Byte*>(dst_buf.get()), static_cast<std::size_t>(dst_size));
    const TransformedJpegInfo transformed = inspectTransformedJpeg(result);

    throwIf(
        transformed.width == 0 || transformed.height == 0,
        "Image Error: Failed to parse transformed JPEG header.");
    if (transformed.width < MIN_IMAGE_DIMENSION ||
        transformed.height < MIN_IMAGE_DIMENSION) {
        throw std::runtime_error(std::format(
            "Image Error: Lossless orientation/MCU trimming reduced dimensions "
            "to {}x{}. Cover image must remain at least {}px for both width and height.",
            transformed.width, transformed.height, MIN_IMAGE_DIMENSION));
    }

    throwIf(
        !transformed.quality,
        "Image File Error: Quantization table referenced by JPEG components "
        "was not found (corrupt or unsupported JPG).");
    if (enforceQualityLimit && *transformed.quality > MAX_ALLOWED_QUALITY) {
        throw std::runtime_error(std::format(
            "Image Error: Estimated quality {} exceeds maximum ({}).\n"
            "For platform compatibility, cover image quality "
            "must be {} or lower.",
            *transformed.quality, MAX_ALLOWED_QUALITY, MAX_ALLOWED_QUALITY));
    }
    throwIf(
        transformed.offset == 0 || transformed.offset >= dst_size,
        "Image File Error: No DQT segment found (corrupt or unsupported JPG).");

    // Report-only: a layout TurboJPEG cannot name (a rotated 4:1:1 cover, say)
    // is described as non-standard rather than refused.
    int out_width = 0, out_height = 0, out_subsamp = -1, out_colorspace = TJCS_YCbCr;
    if (tjDecompressHeader3(transformer.get(), dst_buf.get(), dst_size,
            &out_width, &out_height, &out_subsamp, &out_colorspace) != 0) {
        out_subsamp = -1;
        out_colorspace = TJCS_YCbCr;
    }

    const std::size_t cover_offset = keepAdobeSegment(
        std::span<Byte>(reinterpret_cast<Byte*>(dst_buf.get()), static_cast<std::size_t>(dst_size)),
        transformed.offset);

    // Hand over the turbojpeg buffer itself; the leading markers before the
    // first DQT (bar a kept Adobe segment) are skipped by the offset rather
    // than copied away.
    return OptimizedCover(
        std::move(dst_buf),
        cover_offset,
        static_cast<std::size_t>(dst_size) - cover_offset,
        transformed.offset - cover_offset,
        CoverImageInfo{
            .width = transformed.width,
            .height = transformed.height,
            .quality = *transformed.quality,
            .orientation_applied = xop != static_cast<int>(TJXOP_NONE),
            .progressive = transformed.progressive,
            .color_layout = colorLayoutName(out_subsamp, out_colorspace),
        });
}

} // namespace

OptimizedCover optimizeImage(
    std::span<const Byte> input,
    bool isProgressive,
    bool enforceQualityLimit,
    CoverImageLimits limits) {
    return transformCover(input, validateCoverInput(input, limits), isProgressive, enforceQualityLimit);
}

std::optional<OptimizedCover> normalizeCarrierCover(std::span<const Byte> input, CoverImageLimits limits) {
    const ValidatedCover validated = validateCoverInput(input, limits);
    // No orientation to apply means no flip, rotation or MCU trim; then the
    // checks optimizeImage makes on its output can be made on the input's
    // own frame and tables, and the cover goes to the carrier as it is.
    if (transformOp(validated.prescan) == static_cast<int>(TJXOP_NONE)) {
        const TransformedJpegInfo info = inspectTransformedJpeg(input);
        if (info.width == validated.prescan.width &&
            info.height == validated.prescan.height &&
            info.quality) {
            return std::nullopt;
        }
    }
    return transformCover(input, validated, /*isProgressive=*/false, /*enforceQualityLimit=*/false);
}
