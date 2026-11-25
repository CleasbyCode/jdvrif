// Precondition guards in internal helpers that today's callers never trip:
// each is called here the way a future caller might, and must refuse cleanly
// rather than divide by zero, self-move, overflow or leave errno behind.
// encryption_stream.cpp is compiled in so its internal frame writer is reachable.
#include "encryption_stream.cpp"

#include "carrier_common.h"
#include "carrier_parallel.h"
#include "common.h"
#include "segmentation.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

template<class Action>
bool throws(Action&& action) {
    try {
        action();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void checkCompatiblePlatforms() {
    const vString all{"X-Twitter", "Tumblr", "Mastodon", "Pixelfed", "PostImage", "ImgBB", "ImgPile", "Flickr"};
    expect(compatiblePlatforms({.embedded_image_size = 1024, .first_segment_size = 1024, .total_segments = 1}) == all,
           "a small image fits every platform, in report order");
    expect(compatiblePlatforms({.embedded_image_size = 1024, .first_segment_size = 10 * 1024 + 1, .total_segments = 1}) ==
               vString(all.begin() + 1, all.end()),
           "a first segment over 10 KiB drops X-Twitter only");
    const vString no_mastodon = compatiblePlatforms({.embedded_image_size = 1024, .first_segment_size = 1024, .total_segments = 101});
    expect(std::ranges::find(no_mastodon, "Mastodon") == no_mastodon.end() && no_mastodon.size() == all.size() - 1,
           "more than 100 segments drops Mastodon only");
    expect(compatiblePlatforms({.embedded_image_size = 200uz * 1024 * 1024 + 1, .first_segment_size = 60000, .total_segments = 2}) ==
               vString{"Tumblr"},
           "past Flickr's 200 MiB only Tumblr remains");
    expect(compatiblePlatforms({.embedded_image_size = 200uz * 1024 * 1024 + 1, .first_segment_size = 65535, .total_segments = 2}).empty(),
           "nothing fits an oversized image with a full first segment");
}

void checkQuantTableQualityClamp() {
    using namespace carrier_common;
    volatile int zero = 0;  // keep the call out of constant evaluation
    volatile int over = 250;
    expect(scaledQuantTable(STD_LUMA_Q50, zero) == scaledQuantTable(STD_LUMA_Q50, 1),
           "quality 0 scales like quality 1");
    expect(scaledQuantTable(STD_LUMA_Q50, over) == scaledQuantTable(STD_LUMA_Q50, 100),
           "quality above 100 scales like 100");
}

void checkParallelForEachBounds() {
    const auto noop = [](std::int64_t) {};
    expect(throws([&] { carrier_parallel::forEach(0, 10, 0, noop); }), "chunk 0 is refused");
    expect(throws([&] { carrier_parallel::forEach(0, 10, -4, noop); }), "negative chunk is refused");
    expect(throws([&] {
        carrier_parallel::forEach(0, std::numeric_limits<std::int64_t>::max(), 1 << 20, noop);
    }), "bounds that would wrap the chunk counter are refused");
    std::int64_t sum = 0;
    carrier_parallel::forEach(0, 100, 7, [&](std::int64_t index) {
        static std::mutex mutex;
        const std::lock_guard lock(mutex);
        sum += index;
    });
    expect(sum == 4950, "ordinary loop visits every index once");
}

void checkPositiveSettingKeepsErrno() {
    ::setenv("JDVRIF_TEST_SETTING", "99999999999999999999999999", 1);
    errno = 0;
    const auto value = carrier_parallel::detail::positiveSetting("JDVRIF_TEST_SETTING", false);
    expect(errno == 0, "out-of-range setting leaves errno unchanged");
    expect(value == UINT_MAX, "out-of-range setting clamps to UINT_MAX");
    ::unsetenv("JDVRIF_TEST_SETTING");
}

void checkShortIccTemplateRefused() {
    expect(throws([] { (void)embeddedJpgSize(100, 1000, 4096); }),
           "ICC template shorter than its profile header is refused");
}

void checkFrameWiderThanChunkRefused() {
    // A canary straight after the chunk buffer shows any write past its end.
    struct Guarded {
        CipherChunk chunk{};
        std::array<Byte, 64> canary{};
    };
    auto guarded = std::make_unique<Guarded>();
    guarded->canary.fill(0xA5);

    crypto_secretstream_xchacha20poly1305_state state{};
    std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> header{};
    std::array<unsigned char, crypto_secretstream_xchacha20poly1305_KEYBYTES> key{};
    crypto_secretstream_xchacha20poly1305_init_push(&state, header.data(), key.data());

    const vBytes plain(STREAM_CHUNK_SIZE + 32, 0x11);
    const bool refused = throws([&] {
        pushSecretStreamFrame(state, plain, std::span<const Byte>{}, false, guarded->chunk,
                              [](std::span<const Byte>) {});
    });
    expect(refused, "plain chunk larger than the cipher buffer is refused");
    expect(std::ranges::all_of(guarded->canary, [](Byte b) { return b == 0xA5; }),
           "nothing is written past the cipher buffer");
}

} // namespace

int main() {
    if (sodium_init() < 0) return EXIT_FAILURE;
    checkCompatiblePlatforms();
    checkQuantTableQualityClamp();
    checkParallelForEachBounds();
    checkPositiveSettingKeepsErrno();
    checkShortIccTemplateRefused();
    checkFrameWiderThanChunkRefused();
    if (failures != 0) return EXIT_FAILURE;
    std::puts("internal precondition tests passed.");
    return EXIT_SUCCESS;
}
