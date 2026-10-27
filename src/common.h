#pragma once

// ---------------------------------------------------------------------------
// Toolchain floor. jdvrif is distributed as source and built by each user, so
// fail early with a clear message rather than a wall of template errors when
// the compiler is too old. Features that set this floor:
//   - std::print / std::println          (GCC 14, Clang 18 + libc++ 18)
//   - std::ranges::fold_left, [[assume]] (GCC 13+ / Clang 19)
//   - std::out_ptr                       (libstdc++ 14, libc++ 19)
//   - std::views::zip / chunk / enumerate (libstdc++ 13; not in libc++ 18)
//   - static operator(), static lambdas, uz literals (GCC 13, Clang 16)
// A matching C++23 standard library is required. Clang builds must therefore
// use libstdc++ 14+.
// ---------------------------------------------------------------------------
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 14
#  error "jdvrif requires GCC >= 14 (for std::print, std::ranges::fold_left). Please upgrade your compiler."
#elif defined(__clang__) && __clang_major__ < 18
#  error "jdvrif requires Clang >= 18 with libstdc++ 14+. Please upgrade your compiler."
#endif

#include <sodium.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using Byte = std::uint8_t;
using vBytes = std::vector<Byte>;
using vString = std::vector<std::string>;

using Key = std::array<Byte,  crypto_secretstream_xchacha20poly1305_KEYBYTES>;
using Salt = std::array<Byte, crypto_pwhash_SALTBYTES>;
using StreamHeader = std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>;

// Validation-failure throw helpers, used in place of `if (bad) { throw ...; }`
// blocks throughout. `message` is a constant at every call site, so nothing is
// built unless the failure actually fires. Never pass a std::format() call:
// throwIf's argument is evaluated eagerly, on the success path too.
[[noreturn]] inline void throwError(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

inline void throwIf(bool condition, std::string_view message) {
    if (condition) throwError(message);
}

// Encrypted-payload caps shared by conceal and recover. Recover uses these to
// refuse staged extraction of absurd declared sizes before PIN entry / decrypt.
inline constexpr std::size_t MAX_EMBEDDED_CIPHERTEXT_DEFAULT = 2uz * 1024 * 1024 * 1024;
inline constexpr std::size_t MAX_EMBEDDED_CIPHERTEXT_BLUESKY = 2 * 1024 * 1024;
inline constexpr std::size_t REDDIT_UPLOAD_SIZE_LIMIT = 20uz * 1024 * 1024;
inline constexpr std::size_t TWITTER_UPLOAD_SIZE_LIMIT = 5uz * 1024 * 1024;

struct CoverImageLimits {
    std::uint32_t max_dimension;
    std::uint64_t max_pixels;
};

// Default and Bluesky cover preparation is capped at 4096px per side.
// X-Twitter applies the same platform ceiling independently. Reddit accepts
// larger square images, so its explicitly selected conceal/recover carrier may
// use the platform's 8192x8192 limit.
inline constexpr CoverImageLimits DEFAULT_COVER_IMAGE_LIMITS{
    4'096,
    4'096ULL * 4'096ULL,
};
inline constexpr CoverImageLimits REDDIT_COVER_IMAGE_LIMITS{
    8'192,
    8'192ULL * 8'192ULL,
};

// Recovery accepts a bounded amount of transport overhead beyond conceal's
// advertised limits: multi-ICC files include an 18-byte header per segment.
inline constexpr std::size_t MAX_EMBEDDED_SPAN_RECOVERY_DEFAULT =
    MAX_EMBEDDED_CIPHERTEXT_DEFAULT + 50uz * 1024 * 1024;

// ---- Wiping secrets ---------------------------------------------------------
//
// Two shapes, both sized by sizeof(T) so an element count can never be mistaken
// for a byte count: SecureBuffer owns a value and zeroes it on destruction;
// WipePodGuard zeroes a value the caller owns, on every exit path. Containers
// whose heap capacity can outgrow their size use wipeContainerCapacity instead.

// A value whose bytes are the whole secret: trivially copyable, and not a
// pointer (zeroing a pointer would leave what it points at intact).
template<typename T>
concept WipeableValue = std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>;

template<WipeableValue T>
inline void wipeValue(T& value) noexcept {
    sodium_memzero(std::addressof(value), sizeof(T));
}

template<WipeableValue T>
struct SecureBuffer {
    T buf{};

    SecureBuffer() = default;
    ~SecureBuffer() { wipeValue(buf); }

    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;
    SecureBuffer(SecureBuffer&&) = delete;
    SecureBuffer& operator=(SecureBuffer&&) = delete;
};

template<WipeableValue T>
struct WipePodGuard {
    T* p{nullptr};

    explicit WipePodGuard(T& v) noexcept : p(std::addressof(v)) {}
    WipePodGuard(const WipePodGuard&) = delete;
    WipePodGuard& operator=(const WipePodGuard&) = delete;

    ~WipePodGuard() {
        if (p) wipeValue(*p);
    }
};

// Move-only recovery PIN holder. Zeros its storage on wipe/move-from/destruction
// so intermediate stack copies do not outlive the PIN's intended lifetime.
struct SecurePin {
    enum class Kind : std::uint8_t { none, legacy_decimal, base58_128 };

    Kind kind{Kind::none};
    std::uint64_t legacy_value{0};
    std::array<Byte, 16> secret{};

    SecurePin() = default;

    [[nodiscard]] static SecurePin legacy(std::uint64_t v) noexcept {
        SecurePin pin;
        pin.kind = Kind::legacy_decimal;
        pin.legacy_value = v;
        return pin;
    }

    [[nodiscard]] static SecurePin modern(const std::array<Byte, 16>& s) noexcept {
        SecurePin pin;
        pin.kind = Kind::base58_128;
        pin.secret = s;
        return pin;
    }

    ~SecurePin() { wipe(); }

    SecurePin(const SecurePin&) = delete;
    SecurePin& operator=(const SecurePin&) = delete;

    SecurePin(SecurePin&& other) noexcept
        : kind(other.kind), legacy_value(other.legacy_value), secret(other.secret) {
        other.wipe();
    }

    SecurePin& operator=(SecurePin&& other) noexcept {
        if (this != &other) {
            wipe();
            kind = other.kind;
            legacy_value = other.legacy_value;
            secret = other.secret;
            other.wipe();
        }
        return *this;
    }

    void wipe() noexcept {
        wipeValue(kind);
        wipeValue(legacy_value);
        wipeValue(secret);
    }
};

// A resizable contiguous container of wipeable elements (vBytes, std::string).
template<typename Container>
concept WipeableContainer =
    std::ranges::contiguous_range<Container> &&
    WipeableValue<std::ranges::range_value_t<Container>> &&
    requires(Container& c, std::size_t n) {
        { c.capacity() } -> std::convertible_to<std::size_t>;
        c.resize(n);
        c.clear();
        c.shrink_to_fit();
    };

// Zero the full allocation (capacity, not only size) then release. clear()
// alone leaves backspaced PIN digits / residual ciphertext in the heap freelist.
template<WipeableContainer Container>
inline void wipeContainerCapacity(Container& container) noexcept {
    constexpr std::size_t element_size = sizeof(std::ranges::range_value_t<Container>);
    if (const std::size_t cap = container.capacity(); cap > 0) {
        try {
            // Bring capacity into size so the entire allocation is addressable.
            container.resize(cap);
        } catch (...) {
            // If resize fails, still wipe the live size() region.
            if (!container.empty()) sodium_memzero(container.data(), container.size() * element_size);
            return;
        }
        sodium_memzero(container.data(), cap * element_size);
    }
    container.clear();
    try {
        // shrink_to_fit is a non-binding request that is permitted to
        // reallocate, and therefore to throw. The wipe above has already run,
        // so failing here only means the (now zeroed) allocation is released
        // later by the destructor -- never a reason to terminate the process.
        container.shrink_to_fit();
    } catch (...) {
    }
}

template<WipeableContainer Container>
struct WipeContainerGuard {
    Container* container{nullptr};

    explicit WipeContainerGuard(Container& c) noexcept : container(&c) {}
    WipeContainerGuard(const WipeContainerGuard&) = delete;
    WipeContainerGuard& operator=(const WipeContainerGuard&) = delete;

    ~WipeContainerGuard() {
        if (container) wipeContainerCapacity(*container);
    }
};

using WipeStringGuard = WipeContainerGuard<std::string>;
using WipeBytesGuard = WipeContainerGuard<vBytes>;

inline constexpr std::size_t NO_ZLIB_COMPRESSION_ID_INDEX = 0x80;

inline constexpr Byte NO_ZLIB_COMPRESSION_ID = 0x58;

enum class Mode : Byte {
    conceal,
    recover,
    capsize
};

enum class Option : Byte {
    None,
    Bluesky,
    Reddit,
    Twitter
};

enum class FileTypeCheck : Byte {
    cover_image = 1,
    embedded_image = 2,
    data_file = 3,
    reddit_cover_image = 4,
    twitter_cover_image = 5
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

// Indexes the conceal-mode platform *report* list built by
// platformReportTemplate() in conceal.cpp (X-Twitter, Tumblr, Bluesky,
// Mastodon, Pixelfed, PostImage, ImgBB, ImgPile, Flickr) — NOT the
// PLATFORM_LIMITS table above, which omits Bluesky and is matched by
// name in filterPlatforms(). Keep in sync with the template's ordering.
inline constexpr std::size_t BLUESKY_PLATFORM_INDEX = 2;

inline void requirePlatformEntries(const vString& platforms_vec) {
    throwIf(platforms_vec.size() <= BLUESKY_PLATFORM_INDEX, "Internal Error: Corrupt platform compatibility list.");
}

inline void keepOnlyPlatformEntry(vString& platforms_vec, std::size_t index) {
    requirePlatformEntries(platforms_vec);
    platforms_vec[0] = std::move(platforms_vec[index]);
    platforms_vec.resize(1);
}

inline void removeOptionalPlatformEntries(vString& platforms_vec) {
    requirePlatformEntries(platforms_vec);
    platforms_vec.erase(platforms_vec.begin() + static_cast<std::ptrdiff_t>(BLUESKY_PLATFORM_INDEX));
}
