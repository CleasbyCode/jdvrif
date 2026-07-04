#pragma once

#include "common.h"

#include <span>

[[nodiscard]] std::span<const Byte> defaultIccTemplateBytes() noexcept;
[[nodiscard]] std::span<const Byte> blueskyExifTemplateBytes() noexcept;
[[nodiscard]] std::span<const Byte> photoshopSegmentTemplateBytes() noexcept;
[[nodiscard]] std::span<const Byte> xmpSegmentTemplateBytes() noexcept;
