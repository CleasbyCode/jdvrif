// JPG Data Vehicle (jdvrif v7.5) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
#pragma once

#ifdef _WIN32
    #include "windows/libjpeg-turbo/include/turbojpeg.h"
    #include "windows/zlib-1.3.1/include/zlib.h"

    #define SODIUM_STATIC
    #include "windows/libsodium/include/sodium.h"

    #include <conio.h>

    #define NOMINMAX
    #include <windows.h>
#else
    #include <turbojpeg.h>
    #include <zlib.h>
    #include <sodium.h>

    #include <termios.h>
    #include <unistd.h>
    #include <sys/types.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

using Byte    = std::uint8_t;
using vBytes  = std::vector<Byte>;
using vString = std::vector<std::string>;

using Key   = std::array<Byte, crypto_secretbox_KEYBYTES>;
using Nonce = std::array<Byte, crypto_secretbox_NONCEBYTES>;
using Tag   = std::array<Byte, crypto_secretbox_MACBYTES>;

inline constexpr std::size_t
    NO_ZLIB_COMPRESSION_ID_INDEX = 0x80,
    TAG_BYTES = Tag{}.size();

inline constexpr Byte
    NO_ZLIB_COMPRESSION_ID = 0x58, // 'X'
    PIN_ATTEMPTS_RESET     = 0x90;

enum class Mode   : Byte { conceal, recover };
enum class Option : Byte { None, Bluesky, Reddit };

enum class FileTypeCheck : Byte {
    cover_image    = 1,
    embedded_image = 2,
    data_file      = 3
};

struct PlatformLimits {
    std::string_view name;
    std::size_t      max_image_size;
    std::size_t      max_first_segment;
    uint16_t         max_segments;
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
