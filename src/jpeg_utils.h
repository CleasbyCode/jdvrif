#pragma once

#include "common.h"

#include <span>

struct OptimizedCover {
    vBytes      data;

    [[nodiscard]] std::span<const Byte> view() const noexcept {
        return std::span<const Byte>(data);
    }

    [[nodiscard]] std::size_t trimmed_size() const noexcept {
        return data.size();
    }
};

[[nodiscard]] OptimizedCover optimizeImage(std::span<const Byte> input, bool isProgressive);
