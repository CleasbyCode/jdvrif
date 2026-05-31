#pragma once

// ---------------------------------------------------------------------------
// Toolchain floor. jdvrif is distributed as source and built by each user, so
// fail early with a clear message rather than a wall of template errors when
// the compiler is too old. Features that set this floor:
//   - std::print / std::println          (GCC 14, Clang 18 + libc++ 18)
//   - std::ios::noreplace                (libstdc++ 14, libc++ 18)
//   - std::ranges::fold_left, [[assume]] (GCC 13+ / Clang 19)
// A matching C++23 standard library is required (see also the std::ios::noreplace
// check in file_utils.cpp).
// ---------------------------------------------------------------------------
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 14
#  error "jdvrif requires GCC >= 14 (for std::print, std::ios::noreplace, std::ranges::fold_left). Please upgrade your compiler."
#elif defined(__clang__) && __clang_major__ < 18
#  error "jdvrif requires Clang >= 18 with a C++23 standard library (libc++ 18+ or libstdc++ 14+). Please upgrade your compiler."
#endif

#include <sodium.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using Byte = std::uint8_t;
using vBytes = std::vector<Byte>;
using vString = std::vector<std::string>;

using Key = std::array<Byte, crypto_secretbox_KEYBYTES>;
using Salt = std::array<Byte, crypto_pwhash_SALTBYTES>;

inline constexpr std::size_t NO_ZLIB_COMPRESSION_ID_INDEX = 0x80;

inline constexpr Byte NO_ZLIB_COMPRESSION_ID = 0x58;

enum class Mode : Byte {
    conceal,
    recover
};

enum class Option : Byte {
    None,
    Bluesky,
    Reddit
};

enum class FileTypeCheck : Byte {
    cover_image = 1,
    embedded_image = 2,
    data_file = 3
};

struct PlatformLimits {
    std::string_view name;
    std::size_t max_image_size;
    std::size_t max_first_segment;
    uint16_t max_segments;
};

inline constexpr auto PLATFORM_LIMITS = std::to_array<PlatformLimits>({
    {"X-Twitter",  5   * 1024 * 1024,  10 * 1024,  UINT16_MAX},
    {"Tumblr",     SIZE_MAX,           65534,      UINT16_MAX},
    {"Mastodon",   16  * 1024 * 1024,  SIZE_MAX,   100       },
    {"Pixelfed",   15  * 1024 * 1024,  SIZE_MAX,   UINT16_MAX},
    {"PostImage",  32  * 1024 * 1024,  SIZE_MAX,   UINT16_MAX},
    {"ImgBB",      32  * 1024 * 1024,  SIZE_MAX,   UINT16_MAX},
    {"ImgPile",    100 * 1024 * 1024,  SIZE_MAX,   UINT16_MAX},
    {"Flickr",     200 * 1024 * 1024,  SIZE_MAX,   UINT16_MAX},
});

inline constexpr std::size_t BLUESKY_PLATFORM_INDEX = 2;
inline constexpr std::size_t REDDIT_PLATFORM_INDEX = 5;

inline void requirePlatformEntries(const vString& platforms_vec) {
    if (platforms_vec.size() <= REDDIT_PLATFORM_INDEX) {
        throw std::runtime_error("Internal Error: Corrupt platform compatibility list.");
    }
}

inline void keepOnlyPlatformEntry(vString& platforms_vec, std::size_t index) {
    requirePlatformEntries(platforms_vec);
    platforms_vec[0] = std::move(platforms_vec[index]);
    platforms_vec.resize(1);
}

inline void removeOptionalPlatformEntries(vString& platforms_vec) {
    requirePlatformEntries(platforms_vec);
    platforms_vec.erase(platforms_vec.begin() + static_cast<std::ptrdiff_t>(REDDIT_PLATFORM_INDEX));
    platforms_vec.erase(platforms_vec.begin() + static_cast<std::ptrdiff_t>(BLUESKY_PLATFORM_INDEX));
}
