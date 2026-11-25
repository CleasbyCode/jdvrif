#pragma once

#include "common.h"

#include <span>

class OutputFile;

struct SegmentedEmbedSummary {
    std::size_t embedded_image_size{0};
    uint16_t first_segment_size{0};
    uint16_t total_segments{0};
};

// The platforms known to accept a default-mode image of this layout.
[[nodiscard]] vString compatiblePlatforms(const SegmentedEmbedSummary& summary);

// Exact size of the image EmbeddedJpgWriter produces from a segment template,
// an encrypted payload and a cover of these sizes: the template, the payload
// spread over as many ICC segments as it needs (each with its own header),
// then the cover.
[[nodiscard]] std::size_t embeddedJpgSize(std::size_t template_size, std::size_t encrypted_size, std::size_t jpg_size);

// Writes the default-mode image as the encrypted payload is produced, so the
// ciphertext goes straight into its ICC segments and is never staged in a
// file of its own: SOI, leading_segments (the cover's kept Adobe APP14, if
// any), the ICC segments carrying the template and the payload, then the rest
// of the cover.
//
// The layout is fixed up front from encrypted_size, which the caller knows
// exactly before encrypting (computeStreamEncryptedSizePrefixed); finish()
// refuses a payload that came to any other size.
class EmbeddedJpgWriter {
public:
    EmbeddedJpgWriter(
        OutputFile& output,
        vBytes& segment_vec,
        std::size_t encrypted_size,
        std::span<const Byte> leading_segments);

    EmbeddedJpgWriter(const EmbeddedJpgWriter&) = delete;
    EmbeddedJpgWriter& operator=(const EmbeddedJpgWriter&) = delete;

    // Writes everything ahead of the payload. Call once, after segment_vec
    // holds its final KDF metadata and before the first appendEncrypted().
    void begin();
    // The next bytes of the encrypted payload, in order.
    void appendEncrypted(std::span<const Byte> bytes);
    // Checks the payload is complete, writes the cover (jpg_vec) and reports
    // the finished image.
    [[nodiscard]] SegmentedEmbedSummary finish(std::span<const Byte> jpg_vec);

private:
    void appendPayload(std::span<const Byte> bytes);

    OutputFile& output_;
    vBytes& segment_vec_;
    std::span<const Byte> leading_segments_;
    std::size_t encrypted_size_;
    std::size_t encrypted_left_;
    bool single_segment_{false};
    bool begun_{false};
    // Multi-segment state: payload bytes (template tail + ciphertext) still to
    // place, the room left in the segment being filled, and its index.
    std::size_t payload_left_{0};
    std::size_t segment_room_{0};
    std::size_t segment_index_{0};
    std::size_t segments_required_{0};
    SegmentedEmbedSummary summary_{};
};
