#include "template_assets.h"

// Generated at build time from templates/*.bin (embed_templates.cmake for the
// CMake build). Compiled here, the bytes sit in this object's .rodata and the
// object carries the same CET and non-executable-stack notes as every other.
#include "template_assets_data.h"

#include <type_traits>

static_assert(std::is_same_v<Byte, unsigned char>);

std::span<const Byte> defaultIccTemplateBytes() noexcept {
    return jdvrif_templates::default_icc_template;
}

std::span<const Byte> blueskyExifTemplateBytes() noexcept {
    return jdvrif_templates::bluesky_exif_template;
}

std::span<const Byte> photoshopSegmentTemplateBytes() noexcept {
    return jdvrif_templates::photoshop_segment_template;
}

std::span<const Byte> xmpSegmentTemplateBytes() noexcept {
    return jdvrif_templates::xmp_segment_template;
}
