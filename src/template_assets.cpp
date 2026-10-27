#include "template_assets.h"

extern "C" {
extern const unsigned char _binary_templates_default_icc_template_bin_start[];
extern const unsigned char _binary_templates_default_icc_template_bin_end[];
extern const unsigned char _binary_templates_bluesky_exif_template_bin_start[];
extern const unsigned char _binary_templates_bluesky_exif_template_bin_end[];
extern const unsigned char _binary_templates_photoshop_segment_template_bin_start[];
extern const unsigned char _binary_templates_photoshop_segment_template_bin_end[];
extern const unsigned char _binary_templates_xmp_segment_template_bin_start[];
extern const unsigned char _binary_templates_xmp_segment_template_bin_end[];
}

namespace {
[[nodiscard]] std::span<const Byte> linkedTemplateSpan(
    const unsigned char* start,
    const unsigned char* end) noexcept {
    return {
        reinterpret_cast<const Byte*>(start),
        static_cast<std::size_t>(end - start)
    };
}
} // namespace

std::span<const Byte> defaultIccTemplateBytes() noexcept {
    return linkedTemplateSpan(
        _binary_templates_default_icc_template_bin_start,
        _binary_templates_default_icc_template_bin_end);
}

std::span<const Byte> blueskyExifTemplateBytes() noexcept {
    return linkedTemplateSpan(
        _binary_templates_bluesky_exif_template_bin_start,
        _binary_templates_bluesky_exif_template_bin_end);
}

std::span<const Byte> photoshopSegmentTemplateBytes() noexcept {
    return linkedTemplateSpan(
        _binary_templates_photoshop_segment_template_bin_start,
        _binary_templates_photoshop_segment_template_bin_end);
}

std::span<const Byte> xmpSegmentTemplateBytes() noexcept {
    return linkedTemplateSpan(
        _binary_templates_xmp_segment_template_bin_start,
        _binary_templates_xmp_segment_template_bin_end);
}
