// buildBlueskySegments refuses a ciphertext whose XMP overflow segment would
// exceed its limit before it changes segment_vec at all, and still packs one
// that fits. conceal checks the Bluesky cap first, so this guards the packer
// on its own.
#include "embedded_layout.h"
#include "encryption.h"
#include "encryption_internal.h"
#include "template_assets.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

int main() {
    const auto exif_template = blueskyExifTemplateBytes();
    const vBytes pristine(exif_template.begin(), exif_template.end());

    // Fits: the packer's own largest ciphertext.
    {
        vBytes segment_vec = pristine;
        const vBytes data(maxBlueskyEmbeddedCipherCapacity(), 0x5A);
        buildBlueskySegments(segment_vec, data);
        if (segment_vec.size() <= pristine.size()) {
            std::fputs("FAIL: a fitting ciphertext was not packed\n", stderr);
            return EXIT_FAILURE;
        }
    }
    // Too large: every dataset full, and a whole XMP segment's worth of raw
    // bytes beyond them, which base64 makes larger still.
    {
        vBytes segment_vec = pristine;
        const std::size_t size = BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit +
            BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit +
            BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit +
            BLUESKY_SEGMENT_LAYOUT.xmp_segment_size_limit;
        const vBytes data(size, 0x5A);
        bool refused = false;
        try {
            buildBlueskySegments(segment_vec, data);
        } catch (const std::runtime_error&) {
            refused = true;
        }
        if (!refused) {
            std::fputs("FAIL: an oversized XMP overflow was packed\n", stderr);
            return EXIT_FAILURE;
        }
        if (segment_vec != pristine || segment_vec.capacity() != pristine.size()) {
            std::fputs("FAIL: the refusal changed segment_vec\n", stderr);
            return EXIT_FAILURE;
        }
    }
    std::puts("Bluesky packer refuses an oversized XMP overflow before changing its input.");
    return EXIT_SUCCESS;
}
