// Header-walker edge cases in jpeg_utils.cpp. Its walkers live in an anonymous
// namespace, so this test compiles the file into itself instead of linking it.
#include "jpeg_utils.cpp"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

using Bytes = std::vector<Byte>;

void append(Bytes& out, std::initializer_list<Byte> bytes) { out.insert(out.end(), bytes); }

void appendSegment(Bytes& out, Byte marker, const Bytes& payload) {
    const std::size_t length = payload.size() + 2;
    append(out, {0xFF, marker, static_cast<Byte>(length >> 8), static_cast<Byte>(length & 0xFF)});
    out.insert(out.end(), payload.begin(), payload.end());
}

Bytes sofPayload() {
    // 8-bit, 600x800, three components, the first using table 0.
    return {8, 0x02, 0x58, 0x03, 0x20, 3, 1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1};
}

Bytes dqtPayload() {
    Bytes payload{0x00};
    payload.insert(payload.end(), DQT_8BIT_TABLE_SIZE, 8);
    return payload;
}

// Little-endian TIFF with a single Orientation entry of the given type/count.
Bytes exifWithOrientation(uint16_t type, uint32_t count, uint16_t value) {
    Bytes exif{'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 0x2A, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01};
    append(exif, {static_cast<Byte>(type), static_cast<Byte>(type >> 8)});
    for (int shift = 0; shift < 32; shift += 8) exif.push_back(static_cast<Byte>(count >> shift));
    append(exif, {static_cast<Byte>(value), static_cast<Byte>(value >> 8), 0, 0, 0, 0, 0, 0});
    return exif;
}

Bytes headerWith(const Bytes& before_sof) {
    Bytes jpg{0xFF, 0xD8};
    jpg.insert(jpg.end(), before_sof.begin(), before_sof.end());
    appendSegment(jpg, 0xDB, dqtPayload());
    appendSegment(jpg, 0xC0, sofPayload());
    appendSegment(jpg, 0xDA, {3, 1, 0, 2, 0x11, 3, 0x11, 0, 63, 0});
    return jpg;
}

void checkOrientationNeedsOneShort() {
    auto orientation_of = [](uint16_t type, uint32_t count) {
        Bytes app1;
        appendSegment(app1, 0xE1, exifWithOrientation(type, count, 6));
        const auto scan = jpegPrescan(headerWith(app1));
        return scan ? scan->orientation : std::optional<uint16_t>{};
    };
    expect(orientation_of(3, 1) == std::optional<uint16_t>{6}, "SHORT orientation is read");
    expect(!orientation_of(4, 1), "LONG orientation is ignored");
    expect(!orientation_of(3, 2), "orientation with count 2 is ignored");
}

void checkSoiAndEoiEndTheHeaderWalk() {
    for (const Byte marker : {Byte{0xD8}, Byte{0xD9}}) {
        Bytes stray{0xFF, marker};
        const Bytes jpg = headerWith(stray);
        expect(!jpegPrescan(jpg), "prescan refuses SOI/EOI before SOS");
        const auto info = inspectTransformedJpeg(jpg);
        expect(info.width == 0 && !info.quality, "inspection refuses SOI/EOI before SOS");
    }
    // Standalone markers libjpeg skips are still skipped.
    expect(jpegPrescan(headerWith({0xFF, 0xD3, 0xFF, 0x01})).has_value(), "prescan skips RST/TEM");
}

void checkInspectionNeedsMarkerPrefix() {
    // A bare 0xDB with no 0xFF before it is not a DQT marker.
    Bytes jpg{0xFF, 0xD8, 0xDB, 0x00, 0x43};
    const Bytes dqt = dqtPayload();
    jpg.insert(jpg.end(), dqt.begin(), dqt.end());
    appendSegment(jpg, 0xC0, sofPayload());
    appendSegment(jpg, 0xDA, {3, 1, 0, 2, 0x11, 3, 0x11, 0, 63, 0});
    const auto info = inspectTransformedJpeg(jpg);
    expect(info.offset == 0 && !info.quality, "bare DQT byte is not taken as a marker");
    expect(!jpegPrescan(jpg), "prescan refuses a bare marker byte");
}

void checkAdobeSegmentFoundPastFillAndStandaloneMarkers() {
    Bytes adobe;
    appendSegment(adobe, 0xEE, {'A', 'd', 'o', 'b', 'e', 0, 0x64, 0, 0, 0, 0, 0});
    Bytes jfif;
    appendSegment(jfif, 0xE0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});

    Bytes jpg{0xFF, 0xD8, 0xFF, 0xFF, 0xFF, 0xD0};
    jpg.insert(jpg.end(), adobe.begin(), adobe.end());
    jpg.insert(jpg.end(), jfif.begin(), jfif.end());
    const std::size_t dqt_offset = jpg.size();
    appendSegment(jpg, 0xDB, dqtPayload());

    const std::size_t offset = keepAdobeSegment(jpg, dqt_offset);
    expect(offset == dqt_offset - adobe.size(), "Adobe segment found past fill bytes and RST");
    expect(std::ranges::equal(std::span(jpg).subspan(offset, adobe.size()), adobe),
           "Adobe segment moved to just before the DQT");
}

// countScans: SOS headers up to EOI, never bytes inside a marker payload or
// entropy-coded data, and requireScanLimit refuses past MAX_INPUT_SCANS.
void checkScanCount() {
    Bytes jpg{0xFF, 0xD8};
    Bytes comment;
    for (int i = 0; i < 150; ++i) append(comment, {0xFF, 0xDA});
    appendSegment(jpg, 0xFE, comment);
    appendSegment(jpg, 0xDB, dqtPayload());
    const Bytes sos{1, 1, 0, 0, 63, 0};
    // Entropy data with a stuffed FF, a restart marker and fill bytes.
    const Bytes entropy{0x12, 0xFF, 0x00, 0x34, 0xFF, 0xD3, 0x56, 0xFF, 0xFF};
    for (int scan = 0; scan < 3; ++scan) {
        appendSegment(jpg, 0xDA, sos);
        jpg.insert(jpg.end(), entropy.begin(), entropy.end());
        appendSegment(jpg, 0xC4, {0x10});  // a table between scans
    }
    Bytes ended = jpg;
    append(ended, {0xFF, 0xD9});
    appendSegment(ended, 0xDA, sos);  // after EOI: libjpeg never reads it
    expect(countScans(ended) == 3, "three scans counted, comment and post-EOI bytes not");

    Bytes many{0xFF, 0xD8};
    for (int scan = 0; scan < jpeg_safety::MAX_INPUT_SCANS; ++scan) {
        appendSegment(many, 0xDA, sos);
        many.push_back(0);
    }
    bool threw = false;
    try { requireScanLimit(many); } catch (const std::runtime_error&) { threw = true; }
    expect(!threw, "MAX_INPUT_SCANS scans are allowed");
    appendSegment(many, 0xDA, sos);
    threw = false;
    try { requireScanLimit(many); } catch (const std::runtime_error&) { threw = true; }
    expect(threw, "one scan over MAX_INPUT_SCANS is refused");

    // Truncated or zero-length segments end the count, as they end libjpeg.
    expect(countScans(Bytes{0xFF, 0xD8, 0xFF, 0xDA, 0x00}) == 0, "truncated SOS header");
    expect(countScans(Bytes{0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x00, 0xFF, 0xDA, 0x00, 0x08}) == 0,
           "zero-length segment stops the walk");
}

} // namespace

int main() {
    checkOrientationNeedsOneShort();
    checkSoiAndEoiEndTheHeaderWalk();
    checkInspectionNeedsMarkerPrefix();
    checkAdobeSegmentFoundPastFillAndStandaloneMarkers();
    checkScanCount();
    if (failures != 0) return EXIT_FAILURE;
    std::puts("JPEG header walker tests passed.");
    return EXIT_SUCCESS;
}
