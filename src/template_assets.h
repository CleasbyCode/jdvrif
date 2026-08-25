#pragma once

#include "common.h"

#include <span>

// Templates live in read-only .rodata (see CMakeLists); callers that patch one
// take their own copy through this.
[[nodiscard]] inline vBytes copyTemplateBytes(std::span<const Byte> template_bytes) {
    return vBytes(template_bytes.begin(), template_bytes.end());
}

[[nodiscard]] std::span<const Byte> defaultIccTemplateBytes() noexcept;
[[nodiscard]] std::span<const Byte> blueskyExifTemplateBytes() noexcept;
[[nodiscard]] std::span<const Byte> photoshopSegmentTemplateBytes() noexcept;
[[nodiscard]] std::span<const Byte> xmpSegmentTemplateBytes() noexcept;
