#pragma once

#include "common.h"

#include <span>

class OutputFile;

struct SegmentedEmbedSummary {
    std::size_t embedded_image_size{0};
    uint16_t first_segment_size{0};
    uint16_t total_segments{0};
};

void filterPlatforms(
    vString& platforms_vec,
    std::size_t embedded_size,
    uint16_t first_segment_size,
    uint16_t total_segments);

// Exact size of the image writeEmbeddedJpgFromEncryptedFile produces from a
// segment template, an encrypted payload and a cover of these sizes: the
// template, the payload spread over as many ICC segments as it needs (each
// with its own header), then the cover.
[[nodiscard]] std::size_t embeddedJpgSize(std::size_t template_size, std::size_t encrypted_size, std::size_t jpg_size);

[[nodiscard]] SegmentedEmbedSummary writeEmbeddedJpgFromEncryptedFile(
    OutputFile& output,
    vBytes& segment_vec,
    const fs::path& encrypted_path,
    std::span<const Byte> jpg_vec);
