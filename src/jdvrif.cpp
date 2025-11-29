// JPG Data Vehicle (jdvrif v7.1) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

// Compile program (Linux):

// $ sudo apt-get install libsodium-dev
// $ sudo apt-get install libturbojpeg0-dev

// $ chmod +x compile_jdvrif.sh
// $ ./compile_jdvrif.sh
	
// $ Compilation successful. Executable 'jdvrif' created.
// $ sudo cp jdvrif /usr/bin
// $ jdvrif

#ifdef _WIN32
	#include "windows/libjpeg-turbo/include/turbojpeg.h"
	
	// This software is based in part on the work of the Independent JPEG Group.
	// Copyright (C) 2009-2024 D. R. Commander. All Rights Reserved.
	// Copyright (C) 2015 Viktor Szathmáry. All Rights Reserved.
	// https://github.com/libjpeg-turbo/libjpeg-turbo	
	
	#include "windows/zlib-1.3.1/include/zlib.h"
	
	// zlib.h -- interface of the 'zlib' general purpose compression library
	// version 1.3.1, January 22nd, 2024

	// Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler

	// This software is provided 'as-is', without any express or implied
	// warranty. In no event will the authors be held liable for any damages
	// arising from the use of this software.

	// Permission is granted to anyone to use this software for any purpose,
	// including commercial applications, and to alter it and redistribute it
	// freely, subject to the following restrictions:

	// 1. The origin of this software must not be misrepresented; you must not
	//    claim that you wrote the original software. If you use this software
	//    in a product, an acknowledgment in the product documentation would be
	//    appreciated but is not required.
	// 2. Altered source versions must be plainly marked as such, and must not be
	//    misrepresented as being the original software.
	// 3. This notice may not be removed or altered from any source distribution.

	// Jean-loup Gailly        Mark Adler
	// jloup@gzip.org          madler@alumni.caltech.edu
	
	#define SODIUM_STATIC
	#include "windows/libsodium/include/sodium.h"
	
	// This project uses libsodium (https://libsodium.org/) for cryptographic functions.
	// Copyright (c) 2013-2025 Frank Denis <github@pureftpd.org>
	
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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream> 
#include <iostream>
#include <initializer_list>
#include <iterator> 
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>    

namespace fs = std::filesystem;

using Byte    = std::uint8_t;
using vBytes  = std::vector<Byte>;
using vString = std::vector<std::string>;

using Key   = std::array<Byte, crypto_secretbox_KEYBYTES>;
using Nonce = std::array<Byte, crypto_secretbox_NONCEBYTES>;
using Tag   = std::array<Byte, crypto_secretbox_MACBYTES>;

constexpr std::size_t 
	NO_ZLIB_COMPRESSION_ID_INDEX = 0x80ULL,
	TAG_BYTES = std::tuple_size<Tag>::value;
	
constexpr Byte NO_ZLIB_COMPRESSION_ID = 0x58; // 'X'

enum class Mode   : Byte { conceal, recover };
enum class Option : Byte { None, Bluesky, Reddit };

enum class FileTypeCheck : Byte {
	cover_image    = 1, // Conceal mode...
    embedded_image = 2, // Recover mode...
    data_file 	   = 3  // Conceal mode...
};

static constexpr auto view = [](const auto& container) -> std::span<const Byte> {
	return std::span(container);
};

static void displayInfo() {
	std::cout << R"(

JPG Data Vehicle (jdvrif v7.1)
Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

jdvrif is a metadata “steganography-like” command-line tool used for concealing and extracting
any file type within and from a JPG image.

──────────────────────────
Compile & run (Linux)
──────────────────────────

  $ sudo apt-get install libsodium-dev
  $ sudo apt-get install libturbojpeg0-dev

  $ chmod +x compile_jdvrif.sh
  $ ./compile_jdvrif.sh

  Compilation successful. Executable 'jdvrif' created.

  $ sudo cp jdvrif /usr/bin
  $ jdvrif

──────────────────────────
Usage
──────────────────────────

  jdvrif conceal [-b|-r] <cover_image> <secret_file>
  jdvrif recover <cover_image>
  jdvrif --info

──────────────────────────
Platform compatibility & size limits
──────────────────────────

Share your “file-embedded” JPG image on the following compatible sites.

Platforms where size limit is measured by the combined size of cover image + compressed data file:

	• Flickr    (200 MB)
	• ImgPile   (100 MB)
	• ImgBB     (32 MB)
	• PostImage (32 MB)
	• Reddit    (20 MB) — (use -r option).
	• Pixelfed  (15 MB)

Limit measured by compressed data file size only:

	• Mastodon  (~6 MB)
	• Tumblr    (~64 KB)
	• X-Twitter (~10 KB)

For example, on Mastodon, even if your cover image is 1 MB, you can still embed a data file
up to the ~6 MB Mastodon size limit.

Other: 

Bluesky - Separate size limits for cover image and data file - (use -b option).
  • Cover image: 800 KB
  • Secret data file (compressed): ~171 KB

Even though jdvrif compresses the data file, you may want to compress it yourself first
(zip, rar, 7z, etc.) so that you know the exact compressed file size.

Platforms with small size limits, like X-Twitter (~10 KB), are best suited for data that 
compress especially well, such as text files.

──────────────────────────
Modes
──────────────────────────

conceal - *Compresses, encrypts and embeds your secret data file within a JPG cover image.
recover - Decrypts, uncompresses and extracts the concealed data file from a JPG cover image
          (recovery PIN required).

(*Compression: If data file is already a compressed file type (based on file extension: e.g. ".zip") 
 and the file is greater than 10MB, skip compression). 
		
──────────────────────────
Platform options for conceal mode
──────────────────────────

-b (Bluesky) : Creates compatible “file-embedded” JPG images for posting on Bluesky.

$ jdvrif conceal -b my_image.jpg hidden.doc

These images are only compatible for posting on Bluesky. 

You must use the Python script “bsky_post.py” (in the repo’s src folder) to post to Bluesky.
Posting via the Bluesky website or mobile app will NOT work.

You also need to create an app password for your Bluesky account: https://bsky.app/settings/app-passwords
    
Here are some basic usage examples for the bsky_post.py Python script:

Standard image post to your profile/account.

$ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx 
--image your_image.jpg --alt-text "alt-text here [optional]" "standard post text here [required]"

If you want to post multiple images (Max. 4):

$ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx 
--image img1.jpg --image img2.jpg --alt-text "alt_here" "standard post text..."

If you want to post an image as a reply to another thread:

$ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx 
--image your_image.jpg --alt-text "alt_here" 
--reply-to https://bsky.app/profile/someone.bsky.social/post/8m2tgw6cgi23i 
"standard post text..."

Bluesky size limits: Cover 800 KB / Secret data file (compressed) ~171 KB

-r (Reddit) : Creates compatible “file-embedded” JPG images for posting on Reddit.

$ jdvrif conceal -r my_image.jpg secret.mp3

From the Reddit site, click “Create Post”, then select the “Images & Video” tab to attach the JPG image.
These images are only compatible for posting on Reddit.

To correctly download images from X-Twitter or Reddit, click image within the post to fully expand it before saving.

)"; 
}

struct ProgramArgs {
    Mode mode{Mode::conceal};
    Option option{Option::None};
    fs::path image_file_path;
    fs::path data_file_path;

private:
    [[noreturn]] static void die(const std::string& usage) {
        throw std::runtime_error(usage);
    }

public:
    static std::optional<ProgramArgs> parse(int argc, char** argv) {
        using std::string_view;
        auto arg = [&](int i) -> string_view {
            return (i >= 0 && i < argc) ? string_view(argv[i]) : string_view{};
        };
        const std::string PROG = fs::path(argv[0]).filename().string(), 
            USAGE = "Usage: " + PROG + " conceal [-b|-r] <cover_image> <secret_file>\n\t\b" + 
                PROG + " recover <cover_image>\n\t\b" + 
                PROG + " --info";

        if (argc < 2) die(USAGE);
        if (argc == 2 && arg(1) == "--info") {
            displayInfo();
            return std::nullopt;
        }
        ProgramArgs out{};
        const string_view MODE = arg(1);
        if (MODE == "conceal") {
            int i = 2;
            if (arg(i) == "-b" || arg(i) == "-r") {
                out.option = (arg(i) == "-b") ? Option::Bluesky : Option::Reddit;
                ++i;
            }
            if (i + 1 >= argc || (i + 2) != argc) die(USAGE);
            out.image_file_path = fs::path(arg(i));
            out.data_file_path  = fs::path(arg(i + 1));
            out.mode = Mode::conceal;
            return out;
        }
        if (MODE == "recover") {
            if (argc != 3) die(USAGE);
            out.image_file_path = fs::path(arg(2));
            out.mode = Mode::recover;
            return out;
        }
        die(USAGE);
    }
};

// Default limit of 0 means "Search Whole File". 
// Any other value means "Search ONLY up to this limit".
static std::optional<std::size_t> searchSig(const vBytes& v, std::span<const Byte> sig, std::size_t limit = 0) {   
	auto end_it = (limit == 0 || limit > v.size()) 
    	? v.end() 
    	: v.begin() + limit;

    auto it = std::search(v.begin(), end_it, sig.begin(), sig.end());
    
    if (it == end_it) return std::nullopt;
    return static_cast<std::size_t>(it - v.begin());
}

[[nodiscard]] static std::optional<uint16_t> exifOrientation(const vBytes& jpg) {
	constexpr size_t EXIF_SEARCH_LIMIT = 4096ULL;
	constexpr auto APP1_SIG = std::to_array<Byte>({0xFF, 0xE1});

	auto app1_pos_opt = searchSig(jpg, std::span<const Byte>(APP1_SIG), EXIF_SEARCH_LIMIT);

    if (!app1_pos_opt) return std::nullopt;
    std::size_t pos = *app1_pos_opt;

    if (pos + 4 > jpg.size()) return std::nullopt;

    uint16_t segment_length = (static_cast<uint16_t>(jpg[pos + 2]) << 8) | jpg[pos + 3];
    std::size_t exif_end = pos + 2 + segment_length;

    if (exif_end > jpg.size()) return std::nullopt;

    std::span<const Byte> payload(jpg.data() + pos + 4, segment_length - 2);

    constexpr std::size_t EXIF_HEADER_SIZE = 6ULL;
    constexpr auto EXIF_SIG = std::to_array<Byte>({'E', 'x', 'i', 'f', '\0', '\0'});

    if (payload.size() < EXIF_HEADER_SIZE || 
    	std::memcmp(payload.data(), EXIF_SIG.data(), EXIF_HEADER_SIZE) != 0) {
        return std::nullopt;
    }
    
    std::span<const Byte> tiff_data = payload.subspan(EXIF_HEADER_SIZE);
    
    if (tiff_data.size() < 8) return std::nullopt; 

    bool is_le = false;
    if (tiff_data[0] == 'I' && tiff_data[1] == 'I') is_le = true;      
    else if (tiff_data[0] == 'M' && tiff_data[1] == 'M') is_le = false;
    else return std::nullopt;

    auto read16 = [&](std::size_t offset) -> uint16_t {
    	if (offset + 2 > tiff_data.size()) return 0;
        return is_le ? 
            static_cast<uint16_t>(tiff_data[offset] | (tiff_data[offset + 1] << 8)) :
            static_cast<uint16_t>((tiff_data[offset] << 8) | tiff_data[offset + 1]);
    };

    auto read32 = [&](std::size_t offset) -> uint32_t {
        if (offset + 4 > tiff_data.size()) return 0;
        if (is_le) {
            return static_cast<uint32_t>(tiff_data[offset]) | 
                   (static_cast<uint32_t>(tiff_data[offset + 1]) << 8) | 
                   (static_cast<uint32_t>(tiff_data[offset + 2]) << 16) | 
                   (static_cast<uint32_t>(tiff_data[offset + 3]) << 24);
        } else {
            return (static_cast<uint32_t>(tiff_data[offset]) << 24) | 
                   (static_cast<uint32_t>(tiff_data[offset + 1]) << 16) | 
                   (static_cast<uint32_t>(tiff_data[offset + 2]) << 8) | 
                   static_cast<uint32_t>(tiff_data[offset + 3]);
        }
    };

    if (read16(2) != 0x002A) return std::nullopt;

    uint32_t ifd_offset = read32(4);
    
    if (ifd_offset < 8 || ifd_offset >= tiff_data.size()) return std::nullopt;
    
    uint16_t entry_count = read16(ifd_offset);
    std::size_t current_entry = ifd_offset + 2ULL; 

    constexpr uint16_t TAG_ORIENTATION = 0x0112;
    constexpr std::size_t ENTRY_SIZE = 12ULL;

    for (uint16_t i = 0; i < entry_count; ++i) {
    	if (current_entry + ENTRY_SIZE > tiff_data.size()) return std::nullopt;

        uint16_t tag_id = read16(current_entry);
        
        if (tag_id == TAG_ORIENTATION) {
            return read16(current_entry + 8);
        }
        current_entry += ENTRY_SIZE;
    }
    return std::nullopt;
}

// Helper: Map EXIF orientation (1-8) to TurboJPEG Transform Operations
static int getTransformOp(uint16_t orientation) {
    switch (orientation) {
        case 2: return TJXOP_HFLIP;
        case 3: return TJXOP_ROT180;
        case 4: return TJXOP_VFLIP;
        case 5: return TJXOP_TRANSPOSE;
        case 6: return TJXOP_ROT90;
        case 7: return TJXOP_TRANSVERSE;
        case 8: return TJXOP_ROT270;
        default: return TJXOP_NONE;
    }
}

// TurboJPEG. RAII wrapper for tjhandle (decompressor or compressor)
struct TJHandle {
	tjhandle handle = nullptr;

    TJHandle() = default;

    TJHandle(const TJHandle&) = delete;
   	TJHandle& operator=(const TJHandle&) = delete;

    TJHandle(TJHandle&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    
    TJHandle& operator=(TJHandle&& other) noexcept {
    	if (this != &other) {
        	reset();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    ~TJHandle() {
        reset();
    }

    void reset() {
        if (handle) {
        	tjDestroy(handle);
            handle = nullptr;
        }
    }

    tjhandle get() const { return handle; }
    tjhandle operator->() const { return handle; }
    explicit operator bool() const { return handle != nullptr; }
};

struct TJBuffer {
	unsigned char* data = nullptr;
	~TJBuffer() { if (data) tjFree(data); }
};

// Standard JPEG Luminance Quantization Table (Quality 50) in ZigZag order
static constexpr auto STD_LUMA_QTABLE = std::to_array<Byte>({
	16, 11, 12, 14, 12, 10, 16, 14, 13, 14, 18, 17, 16, 19, 24, 40, 
    26, 24, 22, 22, 24, 49, 35, 37, 29, 40, 58, 51, 61, 60, 57, 51, 
    56, 55, 64, 72, 92, 78, 64, 68, 87, 69, 55, 56, 80, 109, 81, 87, 
    95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99
});

static int estimateImageQuality(const vBytes& jpg) {
	constexpr auto DQT_SIG = std::to_array<Byte>({0xFF, 0xDB});
    
    constexpr size_t DQT_SEARCH_LIMIT = 32768ULL;

    auto dqt_pos_opt = searchSig(jpg, std::span<const Byte>(DQT_SIG), DQT_SEARCH_LIMIT);
    if (!dqt_pos_opt) return 80; 

    std::size_t pos = *dqt_pos_opt;

    if (pos + 4 > jpg.size()) return 80;

    std::size_t 
		length = (static_cast<std::size_t>(jpg[pos + 2]) << 8) | jpg[pos + 3],
    	end    = pos + 2 + length;
    
    if (end > jpg.size()) return 80;

    pos += 4; 

    while (pos < end) {
    	if (pos + 65 > end) break; 
        Byte 
			header 	  = jpg[pos++],
        	precision = (header >> 4) & 0x0F,
        	table_id  = header & 0x0F;
		
        if (precision == 0 && table_id == 0) {
            double total_scale = 0.0;
            
            for (size_t i = 0; i < 64; ++i) {
				double 
					val = static_cast<double>(jpg[pos + i]),
                	std = static_cast<double>(STD_LUMA_QTABLE[i]);
				
                total_scale += (val * 100.0) / std;
            }
        
            total_scale /= 64.0;

            if (total_scale <= 0.0) return 100;
            
            if (total_scale <= 100.0) {
            	return static_cast<int>(200.0 - total_scale) / 2;
            } else {
            	return static_cast<int>(5000.0 / total_scale);
            }
        }   
        pos += 64; 
    }
    return 80; 
}

static void optimizeImage(vBytes& jpg_vec) {
	if (jpg_vec.empty()) {
        throw std::runtime_error("JPG image is empty!");
    }

    TJHandle transformer;
    transformer.handle = tjInitTransform();
    if (!transformer.handle) {
        throw std::runtime_error("tjInitTransform() failed");
    }
  
    int width = 0, height = 0, jpegSubsamp = 0, jpegColorspace = 0;
    if (tjDecompressHeader3(transformer.get(), jpg_vec.data(), static_cast<unsigned long>(jpg_vec.size()), &width, &height, &jpegSubsamp, &jpegColorspace) != 0) {
        throw std::runtime_error(std::string("Image Error: ") + tjGetErrorStr2(transformer.get()));
    }

	if (width < 300 && height < 300) {
        throw std::runtime_error("Image Error: Dimensions are too small.\nFor platform compatibility, cover image must be at least 300px for both width and height.");
    }

    int estimated_quality = estimateImageQuality(jpg_vec);
    if (estimated_quality > 97) {
    	throw std::runtime_error("Image Error: Quality too high. For platform compatibility, cover image quality must be 97 or lower.");
    }
	
    auto ori_opt = exifOrientation(jpg_vec);
    int xop = TJXOP_NONE;
    
    if (ori_opt) {
    	xop = getTransformOp(*ori_opt);
    }

    tjtransform xform;
    std::memset(&xform, 0, sizeof(tjtransform));
    xform.op = xop;
   
    xform.options = TJXOPT_COPYNONE | TJXOPT_TRIM;
	
    TJBuffer dstBuffer; 
    unsigned long dstSize = 0;

    if (tjTransform(transformer.get(), jpg_vec.data(), static_cast<unsigned long>(jpg_vec.size()), 1, &dstBuffer.data, &dstSize, &xform, 0) != 0) {
    	throw std::runtime_error(std::string("tjTransform: ") + tjGetErrorStr2(transformer.get()));
    }

    if (xop == TJXOP_ROT90 || xop == TJXOP_ROT270 || xop == TJXOP_TRANSPOSE || xop == TJXOP_TRANSVERSE) {
        std::swap(width, height);
    }
    jpg_vec.assign(dstBuffer.data, dstBuffer.data + dstSize);
}

// Writes updated values (2, 4 or 8 bytes), such as segments lengths, index/offsets values, PIN, etc. into the relevant vector index location.	
static void updateValue(vBytes& vec, std::size_t index, std::size_t value, Byte bits) {
    // Allow only 16, 32, or 64 bits.
    if (bits != 16 && bits != 32 && bits != 64) {
        throw std::invalid_argument("updateValue: Invalid bit length. Must be 16, 32, or 64.");
    }

    std::size_t bytes_needed = bits / 8;
    
    if (index + bytes_needed > vec.size()) {
        throw std::out_of_range("updateValue: Index out of bounds.");
    }

    // Write new value to vector index location (Big Endian).
    while (bits > 0) {
        bits -= 8;
        vec[index++] = static_cast<Byte>((value >> bits) & 0xFF);
    }
}

static std::size_t getValue(std::span<const Byte> data, std::size_t index, std::size_t length) {
    // Allow only byte length of: 2, 4, or 8.
    if (length != 2 && length != 4 && length != 8) {
        throw std::out_of_range("getValue: Invalid bytes value. 2, 4 or 8 only.");
    }
    
    if (index + length > data.size()) {
        throw std::out_of_range("getValue: Index out of bounds");
    }
        
    std::size_t value = 0;
        
    // Reconstruct value (Big-Endian).
    for (std::size_t i = 0; i < length; ++i) {
        value = (value << 8) | static_cast<std::size_t>(data[index + i]);
    }
    return value; 
}

static bool hasValidFilename(const fs::path& p) {
	if (p.empty()) {
    	return false;
    }
    
    std::string filename = p.filename().string();
    if (filename.empty()) {
    	return false;
    }

    auto validChar = [](unsigned char c) {
    	return std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '@' || c == '%';
 	};
    return std::all_of(filename.begin(), filename.end(), validChar);
}

static bool hasFileExtension(const fs::path& p, std::initializer_list<const char*> exts) {
	auto e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    for (const char* CAND : exts) {
    	std::string c = CAND;
        std::transform(c.begin(), c.end(), c.begin(), [](unsigned char x){ return static_cast<char>(std::tolower(x)); });
        if (e == c) return true;
   	}
    return false;
}

static void segmentDataFile(vBytes& segment_vec, vBytes& data_vec, vBytes& jpg_vec, vString& platforms_vec, bool hasRedditOption) {
	// Default segment_vec uses color profile segment (FFE2) to store data file. 
	constexpr std::size_t
    	SOI_SIG_LENGTH        = 2ULL,
        SEGMENT_SIG_LENGTH    = 2ULL,
        SEGMENT_HEADER_LENGTH = 16ULL,
        LIBSODIUM_MACBYTES    = 16ULL; 

    Byte value_bit_length = 16;
    
	std::size_t 
        segment_data_size 		   = 65519,
		profile_with_data_vec_size = segment_vec.size(),
        max_first_segment_size 	   = segment_data_size + SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH;
        
    // Store the two start_of_image bytes, to be restored later.
    vBytes soi_bytes(segment_vec.begin(), segment_vec.begin() + SOI_SIG_LENGTH);
    
    if (profile_with_data_vec_size > max_first_segment_size) { 
    	// Data file is too large for a single color profile segment, so split data file in to multiple ICC profile segments.
        profile_with_data_vec_size -= LIBSODIUM_MACBYTES;

        std::size_t 
        	remainder_data         = profile_with_data_vec_size % segment_data_size,
            header_overhead        = SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH, // 4.
            segment_remainder_size = (remainder_data > header_overhead) ? remainder_data - header_overhead : 0,
            total_segments         = profile_with_data_vec_size / segment_data_size,
            segments_required      = total_segments + (segment_remainder_size > 0); // Add and additional segment if there is remainder data (usually). 
            
        uint16_t segments_sequence_val = 1;
            
        constexpr std::size_t SEGMENTS_TOTAL_VAL_INDEX = 0x2E0ULL; 
        updateValue(segment_vec, SEGMENTS_TOTAL_VAL_INDEX, segments_required, value_bit_length);

        // Erase the first 20 bytes of segment_vec. Prevents copying duplicate data...
        segment_vec.erase(segment_vec.begin(), segment_vec.begin() + (SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH));

        data_vec.reserve(profile_with_data_vec_size + (segments_required * (SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH)));  
        
        // ICC Profile Header...
        constexpr auto ICC_HEADER_TEMPLATE = std::to_array<Byte>({
            0xFF, 0xE2, 0x00, 0x00,                         
            'I','C','C','_','P','R','O','F','I','L','E',    
            0x00, 0x00,                                     
            0x01                                            
        });
        
        bool isLastSegment = false;
        
        uint16_t 
            default_segment_length = static_cast<uint16_t>(segment_data_size + SEGMENT_HEADER_LENGTH), // 0xFFFF
            last_segment_length    = static_cast<uint16_t>(segment_remainder_size + SEGMENT_HEADER_LENGTH),
            segment_length         = 0;
            
        std::size_t 
            data_size = 0,
            offset    = 0; 
    
        while (segments_required--) {
        	isLastSegment  = (segments_required == 0);
            data_size      = isLastSegment ? (segment_vec.size() - offset) : segment_data_size;
            segment_length = isLastSegment ? last_segment_length : default_segment_length;
            
            auto header = ICC_HEADER_TEMPLATE;
            
            header[2]  = static_cast<Byte>(segment_length >> 8);
            header[3]  = static_cast<Byte>(segment_length & 0xFF);
            
            header[15] = static_cast<Byte>(segments_sequence_val >> 8);
            header[16] = static_cast<Byte>(segments_sequence_val & 0xFF);
            ++segments_sequence_val;

            data_vec.insert(data_vec.end(), header.begin(), header.end());
            data_vec.insert(data_vec.end(), segment_vec.cbegin() + offset, segment_vec.cbegin() + offset + data_size);
        
            offset += data_size;
        }
        vBytes().swap(segment_vec);
        data_vec.insert(data_vec.begin(), soi_bytes.begin(), soi_bytes.begin() + SOI_SIG_LENGTH); // Restore start of image bytes.
        
    } else {
		// Data file fits within a single segment.
        constexpr std::size_t
    		SEGMENT_HEADER_SIZE_INDEX = 0x04ULL,
    		PROFILE_SIZE_INDEX        = 0x16ULL,
    		PROFILE_SIZE_DIFF         = 16ULL;
            
        const std::size_t
            SEGMENT_SIZE         = profile_with_data_vec_size - (SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH),
            PROFILE_SEGMENT_SIZE = SEGMENT_SIZE - PROFILE_SIZE_DIFF;

        updateValue(segment_vec, SEGMENT_HEADER_SIZE_INDEX, SEGMENT_SIZE, value_bit_length);
        updateValue(segment_vec, PROFILE_SIZE_INDEX, PROFILE_SEGMENT_SIZE, value_bit_length);
                    
        data_vec = std::move(segment_vec);
    }   
    constexpr std::size_t
   		DEFLATED_DATA_FILE_SIZE_INDEX = 0x2E2ULL,
   	 	PROFILE_DATA_SIZE             = 851ULL;
        
    value_bit_length = 32; 
        
    updateValue(data_vec, DEFLATED_DATA_FILE_SIZE_INDEX, data_vec.size() - PROFILE_DATA_SIZE, value_bit_length);
                    
    if (hasRedditOption) {  
    	jpg_vec.insert(jpg_vec.begin(), soi_bytes.begin(), soi_bytes.begin() + SOI_SIG_LENGTH); 

		// Important for Reddit. Downloading an embedded image from Reddit can result in a truncated, corrupt data file.
		// Add these padding bytes to prevent the data file from being truncated. The padding bytes will be reduced instead.
       constexpr std::size_t 
			PADDING_SIZE   = 8000ULL,
    		EOI_SIG_LENGTH = 2ULL;

		constexpr Byte 
    		PADDING_START = 33,
    		PADDING_RANGE = 94;

		vBytes padding_vec = { 0xFF, 0xE2, 0x1F, 0x42 };
		padding_vec.reserve(padding_vec.size() + PADDING_SIZE);

		for (std::size_t i = 0; i < PADDING_SIZE; ++i) {
    		padding_vec.emplace_back(PADDING_START + static_cast<Byte>(randombytes_uniform(PADDING_RANGE)));
		}

		jpg_vec.reserve(jpg_vec.size() + padding_vec.size() + data_vec.size());
		jpg_vec.insert(jpg_vec.end() - EOI_SIG_LENGTH, padding_vec.begin(), padding_vec.end());

		vBytes().swap(padding_vec);

		jpg_vec.insert(jpg_vec.end() - EOI_SIG_LENGTH, data_vec.begin() + EOI_SIG_LENGTH, data_vec.end());

		// Just keep the Reddit compatibility report information from the string vector.
		platforms_vec[0] = std::move(platforms_vec[5]);
		platforms_vec.resize(1);
    } else {     
    	// DEFAULT MODE (hasNoOption).
        segment_vec = std::move(data_vec);
        
        // Remove Bluesky and Reddit from the compatibilty report information vector. Not required here.
        platforms_vec.erase(platforms_vec.begin() + 5); 
        platforms_vec.erase(platforms_vec.begin() + 2);
        
        // Size check.
        // If the data file (data_vec) is small (< 20MB), merge it now with jpg_vec
        // If it is a larger file, keep it separate for the later split write. (faster than vector inserting).
        constexpr std::size_t SIZE_THRESHOLD = 20ULL * 1024 * 1024; // 20 MB
        
        if (segment_vec.size() < SIZE_THRESHOLD) {
        	// File is small. Append the cover image to the data file.
        	// Segment_vec has the headers + data_file. jpg_vec has the DQT + image body.
            
        	segment_vec.reserve(segment_vec.size() + jpg_vec.size());
        	segment_vec.insert(segment_vec.end(), jpg_vec.begin(), jpg_vec.end());
           
        	jpg_vec = std::move(segment_vec);
        }
        // Else: Larger file. (>20MB). Leave it in segment_vec for spilt file writing before we write out jpg_vec later.
    }
    vBytes().swap(data_vec);
}

static void binaryToBase64(std::span<const Byte> BINARY_DATA, vBytes& output_vec) {
	const std::size_t
		INPUT_SIZE  = BINARY_DATA.size(),
    	OUTPUT_SIZE = ((INPUT_SIZE + 2) / 3) * 4;

    static constexpr char BASE64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    output_vec.reserve(output_vec.size() + OUTPUT_SIZE);

    for (std::size_t i = 0; i < INPUT_SIZE; i += 3) {
    	const Byte 
        	OCTET_A = BINARY_DATA[i],
            OCTET_B = (i + 1 < INPUT_SIZE) ? BINARY_DATA[i + 1] : 0,
            OCTET_C = (i + 2 < INPUT_SIZE) ? BINARY_DATA[i + 2] : 0;

        const uint32_t TRIPLE = (static_cast<uint32_t>(OCTET_A) << 16) | 
                                (static_cast<uint32_t>(OCTET_B) << 8) | 
                                OCTET_C;

        output_vec.emplace_back(BASE64_TABLE[(TRIPLE >> 18) & 0x3F]);
        output_vec.emplace_back(BASE64_TABLE[(TRIPLE >> 12) & 0x3F]);
        output_vec.emplace_back((i + 1 < INPUT_SIZE) ? BASE64_TABLE[(TRIPLE >> 6) & 0x3F] : '=');
        output_vec.emplace_back((i + 2 < INPUT_SIZE) ? BASE64_TABLE[TRIPLE & 0x3F] : '=');
    }
}

static void appendBase64AsBinary(std::span<const Byte> BASE64_DATA, vBytes& destination_vec) {
    const std::size_t INPUT_SIZE = BASE64_DATA.size();
	
    if (INPUT_SIZE == 0 || (INPUT_SIZE % 4) != 0) {
        throw std::invalid_argument("Base64 input size must be a multiple of 4 and non-empty");
    }

    static constexpr int8_t BASE64_TABLE[256] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, 
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, 
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, 
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    };
	
    auto decode_val = [](Byte C) -> int {
        return BASE64_TABLE[C];
    };

    destination_vec.reserve(destination_vec.size() + (INPUT_SIZE * 3 / 4));

    for (std::size_t i = 0; i < INPUT_SIZE; i += 4) {
        const Byte 
            C0 = BASE64_DATA[i],
            C1 = BASE64_DATA[i + 1],
            C2 = BASE64_DATA[i + 2],
            C3 = BASE64_DATA[i + 3];

        const bool 
            P2 = (C2 == '='),
            P3 = (C3 == '=');

        if (P2 && !P3) {
        	throw std::invalid_argument("Invalid Base64 padding: '==' required when third char is '='");
        }
        if ((P2 || P3) && (i + 4 < INPUT_SIZE)) {
        	throw std::invalid_argument("Padding '=' may only appear in the final quartet");
        }

        const int 
            V0 = decode_val(C0),
            V1 = decode_val(C1),
            V2 = P2 ? 0 : decode_val(C2),
            V3 = P3 ? 0 : decode_val(C3);

        if (V0 < 0 || V1 < 0 || (!P2 && V2 < 0) || (!P3 && V3 < 0)) {
            throw std::invalid_argument("Invalid Base64 character encountered");
        }

        const uint32_t TRIPLE = (static_cast<uint32_t>(V0) << 18) | (static_cast<uint32_t>(V1) << 12) | (static_cast<uint32_t>(V2) << 6) | static_cast<uint32_t>(V3);

        destination_vec.emplace_back(static_cast<Byte>((TRIPLE >> 16) & 0xFF));
        
        if (!P2) destination_vec.emplace_back(static_cast<Byte>((TRIPLE >> 8) & 0xFF));
        if (!P3) destination_vec.emplace_back(static_cast<Byte>(TRIPLE & 0xFF));
    }
}

// Encrypt data file using the Libsodium cryptographic library
static std::size_t encryptDataFile(vBytes& segment_vec, vBytes& data_vec, vBytes& jpg_vec, vString& platforms_vec, std::string& data_filename, bool hasBlueskyOption, bool hasRedditOption) {
	const std::size_t
    	DATA_FILENAME_XOR_KEY_INDEX = hasBlueskyOption ? 0x175 : 0x2FB,
    	DATA_FILENAME_INDEX         = hasBlueskyOption ? 0x161 : 0x2E7,
        SODIUM_KEY_INDEX            = hasBlueskyOption ? 0x18D : 0x313,
        NONCE_KEY_INDEX             = hasBlueskyOption ? 0x1AD : 0x333;
                    
    const Byte DATA_FILENAME_LENGTH = segment_vec[DATA_FILENAME_INDEX - 1];
        
    Byte value_bit_length = 32;
        
    randombytes_buf(segment_vec.data() + DATA_FILENAME_XOR_KEY_INDEX, DATA_FILENAME_LENGTH);

    std::transform(data_filename.begin(), data_filename.begin() + DATA_FILENAME_LENGTH, 
        segment_vec.begin() + DATA_FILENAME_XOR_KEY_INDEX, segment_vec.begin() + DATA_FILENAME_INDEX, 
            [](char a, Byte b) { return static_cast<Byte>(a) ^ b; }
    );    
    
    Key   key{};
    Nonce nonce{};
            
    crypto_secretbox_keygen(key.data());
    randombytes_buf(nonce.data(), nonce.size());

    std::copy(key.begin(), key.end(), segment_vec.begin() + SODIUM_KEY_INDEX);
    std::copy(nonce.begin(), nonce.end(), segment_vec.begin() + NONCE_KEY_INDEX);

    const std::size_t data_length = data_vec.size();
    
    data_vec.resize(data_length + TAG_BYTES);

    if (crypto_secretbox_easy(data_vec.data(), data_vec.data(), data_length, nonce.data(), key.data()) != 0) {
    	sodium_memzero(key.data(),   key.size());
        sodium_memzero(nonce.data(), nonce.size());
        throw std::runtime_error("crypto_secretbox_easy failed");
    }                
        
    segment_vec.reserve(segment_vec.size() + data_vec.size());
  
    vBytes pshop_vec;
    vBytes xmp_vec;
            
    if (hasBlueskyOption) {  
    
    	// Note:  To help increase data storage size for Bluesky, we can spread the data file over three segments. With a max size of around ~171KB.
    	//	  	  Split data file order (if required, depending on data file size): EXIF (1st part ~64KB) --> PHOTOSHOP (2nd part ~64KB) --> XMP (3rd, final part ~43KB).
    	// 	      Segment order stored within JPG image file (for compatibility reasons): EXIF --> XMP --> PHOTOSHOP.
    	// 	      If the data file size is large enough to require all three segments, XMP, even though containing the last part of the file, will be appended 2nd after EXIF, before Photoshop.
    	//	      EXIF & Photoshop segments store data file parts as binary, XMP stores the data as Base64.
    	
    	constexpr std::size_t 
    		COMPRESSED_FILE_SIZE_INDEX     = 0x1CDULL,
    		EXIF_SEGMENT_DATA_SIZE_LIMIT   = 65027ULL,
    		EXIF_SEGMENT_DATA_INSERT_INDEX = 0x1D1ULL,
    		EXIF_SEGMENT_SIZE_INDEX        = 0x04ULL,   
        	ARTIST_FIELD_SIZE_INDEX        = 0x4AULL,
        	ARTIST_FIELD_SIZE_DIFF	       = 140ULL,
        	FIRST_MARKER_BYTES_SIZE        = 4ULL; 	// FFD8 FFE1
        		
        const std::size_t 
        	ENCRYPTED_VEC_SIZE     = data_vec.size(),
        	SEGMENT_VEC_DATA_SIZE  = segment_vec.size() - FIRST_MARKER_BYTES_SIZE,
        	EXIF_SEGMENT_DATA_SIZE = ENCRYPTED_VEC_SIZE > EXIF_SEGMENT_DATA_SIZE_LIMIT ? EXIF_SEGMENT_DATA_SIZE_LIMIT + SEGMENT_VEC_DATA_SIZE : ENCRYPTED_VEC_SIZE + SEGMENT_VEC_DATA_SIZE,
        	ARTIST_FIELD_SIZE      = EXIF_SEGMENT_DATA_SIZE - ARTIST_FIELD_SIZE_DIFF;
        
        bool hasXmpSegment = false;
        
        updateValue(segment_vec, COMPRESSED_FILE_SIZE_INDEX, ENCRYPTED_VEC_SIZE, value_bit_length);
	    
        if (ENCRYPTED_VEC_SIZE <= EXIF_SEGMENT_DATA_SIZE_LIMIT) {
        	// All data fits in EXIF segment
        	updateValue(segment_vec, ARTIST_FIELD_SIZE_INDEX, ARTIST_FIELD_SIZE, value_bit_length); 
	
			value_bit_length = 16;
    		updateValue(segment_vec, EXIF_SEGMENT_SIZE_INDEX , EXIF_SEGMENT_DATA_SIZE, value_bit_length);
    		
        	segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, data_vec.begin(), data_vec.end());
        } else {
        	// Data overflows EXIF segment — need Photoshop segment with up to 2 datasets, ~32KB each, and possibly an XMP segment (XMP data stored as base64).
        	segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, data_vec.begin(), data_vec.begin() + EXIF_SEGMENT_DATA_SIZE_LIMIT);

            constexpr std::size_t
            	FIRST_DATASET_SIZE_LIMIT = 32767ULL,
                LAST_DATASET_SIZE_LIMIT  = 32730ULL,
                FIRST_DATASET_SIZE_INDEX = 0x21ULL;
               
            value_bit_length = 16;
            
            // Initialize Photoshop segment header and reserve max possible size
            pshop_vec = { 
            	0xFF, 0xED, 0xFF, 0xFF, 0x50, 0x68, 0x6F, 0x74, 0x6F, 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E, 
            	0x30, 0x00, 0x38, 0x42, 0x49, 0x4D, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE3, 0x1C, 0x08, 
            	0x0A, 0x7F, 0xFF
            };
         
            std::size_t 
            	remaining_data_size = ENCRYPTED_VEC_SIZE - EXIF_SEGMENT_DATA_SIZE_LIMIT,
                data_file_index     = EXIF_SEGMENT_DATA_SIZE_LIMIT;
            
            pshop_vec.reserve(pshop_vec.size() + remaining_data_size);         
            
            // First Photoshop dataset
            const std::size_t FIRST_COPY_SIZE = std::min(FIRST_DATASET_SIZE_LIMIT, remaining_data_size);
            
            if (FIRST_DATASET_SIZE_LIMIT > FIRST_COPY_SIZE) {
            	updateValue(pshop_vec, FIRST_DATASET_SIZE_INDEX, FIRST_COPY_SIZE, value_bit_length);
            }
            
            pshop_vec.insert(pshop_vec.end(), data_vec.begin() + data_file_index, data_vec.begin() + data_file_index + FIRST_COPY_SIZE);
                        
            if (remaining_data_size > FIRST_DATASET_SIZE_LIMIT) {
            	remaining_data_size -= FIRST_DATASET_SIZE_LIMIT;
                
            	data_file_index += FIRST_DATASET_SIZE_LIMIT;
                
            	// Second Photoshop dataset
            	const std::size_t LAST_COPY_SIZE = std::min(LAST_DATASET_SIZE_LIMIT, remaining_data_size);
                
                // Inline the dataset marker with size bytes
                const Byte DATASET_MARKER[] = { 
                	0x1C, 0x08, 0x0A, 
                	static_cast<Byte>((LAST_COPY_SIZE >> 8) & 0xFF),
                	static_cast<Byte>(LAST_COPY_SIZE & 0xFF)
                };
				
                pshop_vec.insert(pshop_vec.end(), std::begin(DATASET_MARKER), std::end(DATASET_MARKER));
                pshop_vec.insert(pshop_vec.end(), data_vec.begin() + data_file_index, data_vec.begin() + data_file_index + LAST_COPY_SIZE);
                            
                if (remaining_data_size > LAST_DATASET_SIZE_LIMIT) {   
                	hasXmpSegment = true;
                    
                    remaining_data_size -= LAST_DATASET_SIZE_LIMIT;
                    data_file_index 	+= LAST_DATASET_SIZE_LIMIT;
                    
                    // XMP segment header to store base64-encoded data. Last part of data file.
                    xmp_vec = { 
                    	0xFF, 0xE1, 0x01, 0x93, 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F, 0x2F, 0x6E, 0x73, 0x2E, 0x61, 0x64, 
                        0x6F, 0x62, 0x65, 0x2E, 0x63, 0x6F, 0x6D, 0x2F, 0x78, 0x61, 0x70, 0x2F, 0x31, 0x2E, 0x30, 0x2F, 
                        0x00, 0x3C, 0x3F, 0x78, 0x70, 0x61, 0x63, 0x6B, 0x65, 0x74, 0x20, 0x62, 0x65, 0x67, 0x69, 0x6E, 
                        0x3D, 0x22, 0x22, 0x20, 0x69, 0x64, 0x3D, 0x22, 0x57, 0x35, 0x4D, 0x30, 0x4D, 0x70, 0x43, 0x65, 
                        0x68, 0x69, 0x48, 0x7A, 0x72, 0x65, 0x53, 0x7A, 0x4E, 0x54, 0x63, 0x7A, 0x6B, 0x63, 0x39, 0x64, 
                        0x22, 0x3F, 0x3E, 0x0A, 0x3C, 0x78, 0x3A, 0x78, 0x6D, 0x70, 0x6D, 0x65, 0x74, 0x61, 0x20, 0x78, 
                        0x6D, 0x6C, 0x6E, 0x73, 0x3A, 0x78, 0x3D, 0x22, 0x61, 0x64, 0x6F, 0x62, 0x65, 0x3A, 0x6E, 0x73, 
                        0x3A, 0x6D, 0x65, 0x74, 0x61, 0x2F, 0x22, 0x20, 0x78, 0x3A, 0x78, 0x6D, 0x70, 0x74, 0x6B, 0x3D, 
                        0x22, 0x47, 0x6F, 0x20, 0x58, 0x4D, 0x50, 0x20, 0x53, 0x44, 0x4B, 0x20, 0x31, 0x2E, 0x30, 0x22, 
                        0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x52, 0x44, 0x46, 0x20, 0x78, 0x6D, 0x6C, 0x6E, 0x73, 0x3A, 
                        0x72, 0x64, 0x66, 0x3D, 0x22, 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F, 0x2F, 0x77, 0x77, 0x77, 0x2E, 
                        0x77, 0x33, 0x2E, 0x6F, 0x72, 0x67, 0x2F, 0x31, 0x39, 0x39, 0x39, 0x2F, 0x30, 0x32, 0x2F, 0x32, 
                        0x32, 0x2D, 0x72, 0x64, 0x66, 0x2D, 0x73, 0x79, 0x6E, 0x74, 0x61, 0x78, 0x2D, 0x6E, 0x73, 0x23, 
                        0x22, 0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x44, 0x65, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x69, 
                        0x6F, 0x6E, 0x20, 0x78, 0x6D, 0x6C, 0x6E, 0x73, 0x3A, 0x64, 0x63, 0x3D, 0x22, 0x68, 0x74, 0x74, 
                        0x70, 0x3A, 0x2F, 0x2F, 0x70, 0x75, 0x72, 0x6C, 0x2E, 0x6F, 0x72, 0x67, 0x2F, 0x64, 0x63, 0x2F, 
                        0x65, 0x6C, 0x65, 0x6D, 0x65, 0x6E, 0x74, 0x73, 0x2F, 0x31, 0x2E, 0x31, 0x2F, 0x22, 0x20, 0x72, 
                        0x64, 0x66, 0x3A, 0x61, 0x62, 0x6F, 0x75, 0x74, 0x3D, 0x22, 0x22, 0x3E, 0x3C, 0x64, 0x63, 0x3A, 
                        0x63, 0x72, 0x65, 0x61, 0x74, 0x6F, 0x72, 0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x53, 0x65, 0x71, 
                        0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69, 0x3E
                    };
                    
                    constexpr std::size_t
                    	XMP_SEGMENT_SIZE_LIMIT = 60033ULL,  // Bluesky will strip XMP data lengths greater than 60031 bytes (0xEA7F).
                    	XMP_FOOTER_SIZE        = 92ULL;
                    
                    const std::size_t BASE64_SIZE = ((remaining_data_size + 2) / 3) * 4;
                    xmp_vec.reserve(xmp_vec.size() + BASE64_SIZE + XMP_FOOTER_SIZE);
                    
                    // Encode remaining data as base64 and append directly
                    std::span<const Byte> REMAINING_DATA(data_vec.data() + data_file_index, remaining_data_size);
                    binaryToBase64(REMAINING_DATA, xmp_vec);
                    
                    // Append XMP footer
                    constexpr Byte XMP_FOOTER[XMP_FOOTER_SIZE] = {
                    	0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69, 0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x53, 
                        0x65, 0x71, 0x3E, 0x3C, 0x2F, 0x64, 0x63, 0x3A, 0x63, 0x72, 0x65, 0x61, 0x74, 0x6F, 0x72, 0x3E, 
                        0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x44, 0x65, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x69, 0x6F, 
                        0x6E, 0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x52, 0x44, 0x46, 0x3E, 0x3C, 0x2F, 0x78, 0x3A, 
                        0x78, 0x6D, 0x70, 0x6D, 0x65, 0x74, 0x61, 0x3E, 0x0A, 0x3C, 0x3F, 0x78, 0x70, 0x61, 0x63, 0x6B, 
                        0x65, 0x74, 0x20, 0x65, 0x6E, 0x64, 0x3D, 0x22, 0x77, 0x22, 0x3F, 0x3E
                    };
                    xmp_vec.insert(xmp_vec.end(), std::begin(XMP_FOOTER), std::end(XMP_FOOTER));
                    
                    if (xmp_vec.size() > XMP_SEGMENT_SIZE_LIMIT) {
    		     		// Data file too large for the final available segment. No where to go, so exit the program. Data file exceeds size limit for Bluesky.
            			throw std::runtime_error("File Size Error: Data file exceeds segment size limit for Bluesky.");
        	    	}  
                }  
            }  
        }
        constexpr std::size_t 
        	PSHOP_VEC_DEFAULT_SIZE    = 35ULL,   // PSHOP segment default size before data file.
        	SEGMENT_MARKER_BYTES_SIZE = 2ULL,    // FFE1 or FFED.
        	SEGMENT_SIZE_INDEX	  	  = 0x2ULL,
    		BIM_SECTION_SIZE_INDEX    = 0x1CULL,
    		BIM_SECTION_SIZE_DIFF     = 28ULL;   // Consistant size difference between PSHOP segment size and 8BIM size
    	
    	if (hasXmpSegment) {
			// Update and write/append the xmp segment after EXIF (segment_vec), before Photoshop.
			updateValue(xmp_vec, SEGMENT_SIZE_INDEX, xmp_vec.size() - SEGMENT_MARKER_BYTES_SIZE, value_bit_length);
        	segment_vec.insert(segment_vec.end(), xmp_vec.begin(), xmp_vec.end());
        	std::vector<Byte>().swap(xmp_vec);
        }
    		
        const std::size_t PSHOP_VEC_SIZE = pshop_vec.size();
          
        if (PSHOP_VEC_SIZE > PSHOP_VEC_DEFAULT_SIZE) {
    		const std::size_t 
        		PSHOP_SEGMENT_DATA_SIZE = PSHOP_VEC_SIZE - SEGMENT_MARKER_BYTES_SIZE,
        		BIM_SECTION_SIZE 		= PSHOP_SEGMENT_DATA_SIZE - BIM_SECTION_SIZE_DIFF;
                
            if (!hasXmpSegment) { 
				// If there was no xmp segment, then we need to update the photoshop segment sizes. 
				// We always use default max sizes when there is an xmp segment, so no need to update them when we have xmp.
        		updateValue(pshop_vec, SEGMENT_SIZE_INDEX, PSHOP_SEGMENT_DATA_SIZE, value_bit_length);
       	       	updateValue(pshop_vec, BIM_SECTION_SIZE_INDEX, BIM_SECTION_SIZE, value_bit_length);
            } 
			// Always write/append the photoshop segment last. EXIF --> XMP --> Photoshop (or if no xmp) EXIF --> Photoshop.
            segment_vec.insert(segment_vec.end(), pshop_vec.begin(), pshop_vec.end());
            std::vector<Byte>().swap(pshop_vec);
    	}     
    } else { 
    	// Non-Bluesky: just append data to segment_vec (ICC Profile).
    	segment_vec.insert(segment_vec.end(), data_vec.begin(), data_vec.end());
    }
    
    vBytes().swap(data_vec);
    
    constexpr std::size_t SODIUM_XOR_KEY_LENGTH = 8ULL; 
                   
    std::size_t 
    	pin                = getValue(view(segment_vec), SODIUM_KEY_INDEX, SODIUM_XOR_KEY_LENGTH),
        sodium_keys_length = 48,  
        sodium_xor_key_pos = SODIUM_KEY_INDEX,
        sodium_key_pos     = SODIUM_KEY_INDEX;
  
    sodium_key_pos += SODIUM_XOR_KEY_LENGTH; 
    
    while (sodium_keys_length--) {   
    	segment_vec[sodium_key_pos] ^= segment_vec[sodium_xor_key_pos++];
        sodium_key_pos++;
        if (sodium_xor_key_pos >= SODIUM_KEY_INDEX + SODIUM_XOR_KEY_LENGTH) {
            sodium_xor_key_pos = SODIUM_KEY_INDEX;
        }
    }
                
    std::size_t random_val;
    randombytes_buf(&random_val, sizeof random_val);
            
    value_bit_length = 64;
    updateValue(segment_vec, SODIUM_KEY_INDEX, random_val, value_bit_length);
            
    sodium_memzero(key.data(), key.size());
    sodium_memzero(nonce.data(), nonce.size());
       
    if (hasBlueskyOption) {
    	jpg_vec.reserve(jpg_vec.size() + segment_vec.size());    
    	jpg_vec.insert(jpg_vec.begin(), segment_vec.begin(), segment_vec.end());
    
    	std::vector<Byte>().swap(segment_vec);
    
    	// Just keep the Bluesky compatibilty report information from the string vector.
    	platforms_vec[0] = std::move(platforms_vec[2]);
    	platforms_vec.resize(1);
    } else {
    	segmentDataFile(segment_vec, data_vec, jpg_vec, platforms_vec, hasRedditOption);
    }         
    return pin;
}

static std::size_t getPin() {
	const std::string MAX_UINT64_STR = "18446744073709551615";
	
	std::cout << "\nPIN: ";
    std::string input;
    char ch; 
    bool sync_status = std::cout.sync_with_stdio(false);
	
	#ifdef _WIN32
		while (input.length() < 20) { 
	 		ch = _getch();
        	if (ch >= '0' && ch <= '9') {
            	input.push_back(ch);
            	std::cout << '*' << std::flush;  
        	} else if (ch == '\b' && !input.empty()) {  
            	std::cout << "\b \b" << std::flush;  
            	input.pop_back();
        	} else if (ch == '\r') {
            	break;
        	}
    	}
	#else
		struct termios oldt, newt;
    	tcgetattr(STDIN_FILENO, &oldt);
    	newt = oldt;
    	newt.c_lflag &= ~(ICANON | ECHO);
    	tcsetattr(STDIN_FILENO, TCSANOW, &newt);
	
   		while (input.length() < 20) {
        	ssize_t bytes_read = read(STDIN_FILENO, &ch, 1); 
        	if (bytes_read <= 0) continue; 
       
        	if (ch >= '0' && ch <= '9') {
            	input.push_back(ch);
            	std::cout << '*' << std::flush; 
        	} else if ((ch == '\b' || ch == 127) && !input.empty()) {  
            	std::cout << "\b \b" << std::flush;
            	input.pop_back();
        	} else if (ch == '\n') {
            	break;
        	}
    	}
    	tcsetattr(STDIN_FILENO, TCSANOW, &oldt); 
	#endif

    std::cout << std::endl; 
   	std::cout.sync_with_stdio(sync_status);
		
    if (input.empty() || (input.length() == 20 && input > MAX_UINT64_STR)) {
    	return 0; 
    } else {
    	return std::stoull(input);
    }
}

// Decrypt embedded data file using the Libsodium cryptographic library.
static std::string decryptDataFile(vBytes& jpg_vec, bool isBlueskyFile, bool& hasDecryptionFailed) {
	constexpr std::size_t SODIUM_XOR_KEY_LENGTH = 8ULL; 
	
	const std::size_t 
		SODIUM_KEY_INDEX 	 	 = isBlueskyFile ? 0x18D : 0x2FB,
		NONCE_KEY_INDEX  	 	 = isBlueskyFile ? 0x1AD : 0x31B,
		ENCRYPTED_FILENAME_INDEX = isBlueskyFile ? 0x161 : 0x2CF,
		FILENAME_XOR_KEY_INDEX   = isBlueskyFile ? 0x175 : 0x2E3,
		FILE_SIZE_INDEX 	 	 = isBlueskyFile ? 0x1CD : 0x2CA,
		FILENAME_LENGTH_INDEX    = ENCRYPTED_FILENAME_INDEX - 1;
	
	std::size_t 
		recovery_pin 	   = getPin(),
		byte_length 	   = 2,
		sodium_keys_length = 48,
		sodium_xor_key_pos = SODIUM_KEY_INDEX,
		sodium_key_pos 	   = SODIUM_KEY_INDEX + SODIUM_XOR_KEY_LENGTH;
	
	Byte value_bit_length = 64;
	
	updateValue(jpg_vec, SODIUM_KEY_INDEX, recovery_pin, value_bit_length);
				
	while(sodium_keys_length--) {
		jpg_vec[sodium_key_pos] ^= jpg_vec[sodium_xor_key_pos++];
		sodium_key_pos++;
		if (sodium_xor_key_pos >= SODIUM_KEY_INDEX + SODIUM_XOR_KEY_LENGTH) {
			sodium_xor_key_pos = SODIUM_KEY_INDEX;
		}
	}

	Key   key{};
	Nonce nonce{};
			
	std::copy_n(jpg_vec.begin() + SODIUM_KEY_INDEX, key.size(), key.data());
	std::copy_n(jpg_vec.begin() + NONCE_KEY_INDEX, nonce.size(), nonce.data());
		
    const Byte FILENAME_LENGTH = jpg_vec[FILENAME_LENGTH_INDEX];

	std::string decrypted_filename;
 	decrypted_filename.resize(FILENAME_LENGTH);

    for (std::size_t i = 0; i < FILENAME_LENGTH; ++i) {
    	decrypted_filename[i] = static_cast<char>(jpg_vec[ENCRYPTED_FILENAME_INDEX + i] ^ jpg_vec[FILENAME_XOR_KEY_INDEX + i]);
    }
			
	constexpr std::size_t 
		TOTAL_PROFILE_HEADER_SEGMENTS_INDEX = 0x2C8ULL,
		COMMON_DIFF_VAL 		    		= 65537ULL; // ICC segment spacing. Size difference between each segment profile header.
		
	const uint16_t TOTAL_PROFILE_HEADER_SEGMENTS = static_cast<uint16_t>(getValue(view(jpg_vec), TOTAL_PROFILE_HEADER_SEGMENTS_INDEX, byte_length));		
	
	byte_length = 4;
	
	const std::size_t 
		ENCRYPTED_FILE_START_INDEX = isBlueskyFile ? 0x1D1 : 0x33B,
		EMBEDDED_FILE_SIZE 	   	   = getValue(view(jpg_vec), FILE_SIZE_INDEX, byte_length),
		LAST_SEGMENT_INDEX 	   	   = (static_cast<std::size_t>(TOTAL_PROFILE_HEADER_SEGMENTS) - 1) * COMMON_DIFF_VAL - 0x16;
	
	// Check embedded data file for corruption, such as missing data segments.
	// Why? If you post an embedded image to Mastodon, which exceeds the icc profile segment limit of 100 (~6MB), 
	// it will allow the post if it does not exceed the image size limit (16MB), but it will truncate the segments over 100.
	// When the image is downloaded and an attempt is made to recover the embedded file (jdvrif recover), it will fail because of the missing segments.
	// Also, general corruption could cause this.
	
	if (TOTAL_PROFILE_HEADER_SEGMENTS && !isBlueskyFile) {
		if (LAST_SEGMENT_INDEX >= jpg_vec.size() || jpg_vec[LAST_SEGMENT_INDEX] != 0xFF || jpg_vec[LAST_SEGMENT_INDEX + 1] != 0xE2) {
			throw std::runtime_error("File Extraction Error: Missing segments detected. Embedded data file is corrupt!");
		}
	}
	
    // Copy the wanted bytes to the front, then resize.
	std::memmove(jpg_vec.data(), jpg_vec.data() + ENCRYPTED_FILE_START_INDEX, EMBEDDED_FILE_SIZE);
	jpg_vec.resize(EMBEDDED_FILE_SIZE);
	
	bool hasNoProfileHeaders = (isBlueskyFile || !TOTAL_PROFILE_HEADER_SEGMENTS);
			
	if (hasNoProfileHeaders) {		
		if (crypto_secretbox_open_easy(jpg_vec.data(), jpg_vec.data(), jpg_vec.size(), nonce.data(), key.data()) !=0 ) {
        	std::cerr << "\nDecryption failed!\n";
        	hasDecryptionFailed = true;
        	sodium_memzero(key.data(),   key.size());
        	sodium_memzero(nonce.data(), nonce.size());
        	return {};
    	} 	
	} else {
		constexpr std::size_t 
			PROFILE_HEADER_LENGTH = 18ULL,
			HEADER_INDEX 		  = 0xFCB0ULL; // The first split segment profile header location, this is after the main header/icc profile, which was previously removed.
		
		const std::size_t LIMIT = jpg_vec.size();
			
		std::size_t  
			read_pos    = 0,                 
			write_pos   = 0,                 
			next_header = HEADER_INDEX;		
			
		// We need to avoid including the icc segment profile headers within the decrypted output file.
		// Because we know the total number of profile headers and their location (common difference val), 
		// we can just skip the header when writing the data back to the vector.
        // This is much faster than having to search for and then using something like vec.erase to remove the header string from the vector.

		while (read_pos < LIMIT) {
    		if (read_pos == next_header) {
        		// Skip the header bytes.
        		read_pos 	+= std::min(PROFILE_HEADER_LENGTH, LIMIT - read_pos);
        		next_header += COMMON_DIFF_VAL;
        		continue;
    		}
    		jpg_vec[write_pos++] = jpg_vec[read_pos++];  
		}
		
		jpg_vec.resize(write_pos);               
		jpg_vec.shrink_to_fit();      
			
		if (crypto_secretbox_open_easy(jpg_vec.data(), jpg_vec.data(), jpg_vec.size(), nonce.data(), key.data()) !=0) {
        	std::cerr << "\nDecryption failed!\n";
        	hasDecryptionFailed = true;
        	sodium_memzero(key.data(), key.size());
        	sodium_memzero(nonce.data(), nonce.size());
        	return {};
    	} 
	}
	
	jpg_vec.resize(jpg_vec.size() - TAG_BYTES);
    jpg_vec.shrink_to_fit();
	
	sodium_memzero(key.data(), key.size());
    sodium_memzero(nonce.data(), nonce.size());
	
	return decrypted_filename;
}

// Zlib function, deflate or inflate data file within vector.
static void zlibFunc(vBytes& data_vec, Mode mode) {
	constexpr std::size_t BUFSIZE = 2ULL * 1024 * 1024; // 2 MB.  
    vBytes buffer_vec(BUFSIZE);  
    vBytes output_vec;

    const std::size_t input_size = data_vec.size();
    output_vec.reserve(input_size + BUFSIZE);

    z_stream strm{};
    strm.next_out = buffer_vec.data();
    strm.avail_out = static_cast<uInt>(BUFSIZE);  

    auto flush_buffer = [&]() {
    	const std::size_t written = BUFSIZE - strm.avail_out;
        if (written > 0) {
        	output_vec.insert(output_vec.end(), buffer_vec.begin(), buffer_vec.begin() + written);
            strm.next_out = buffer_vec.data();
            strm.avail_out = static_cast<uInt>(BUFSIZE);
        }
    };

    // --- COMPRESSION ---
    if (mode == Mode::conceal) {
    	int8_t COMPRESSION_LEVEL {};
        		
    	if (input_size > 500ULL * 1024 * 1024) {
    		COMPRESSION_LEVEL = Z_BEST_SPEED;
    	} else if ( input_size > 250ULL * 1024 * 1024 ) {
    		COMPRESSION_LEVEL = Z_DEFAULT_COMPRESSION;
    	} else {
    		COMPRESSION_LEVEL = Z_BEST_COMPRESSION;
    	}
    	
        if (deflateInit(&strm, COMPRESSION_LEVEL) != Z_OK) {
            throw std::runtime_error("zlib: deflateInit failed");
        }

        strm.next_in = data_vec.data();
        std::size_t input_left = input_size;
        strm.avail_in = 0;

        int ret;
        do {
            if (strm.avail_in == 0 && input_left > 0) {
                const std::size_t chunk = std::min(input_left, static_cast<std::size_t>(std::numeric_limits<uInt>::max()));
                strm.avail_in = static_cast<uInt>(chunk);
                input_left -= chunk;
            }

            ret = deflate(&strm, input_left > 0 ? Z_NO_FLUSH : Z_FINISH);

            if (strm.avail_out == 0) {
                flush_buffer();
            }

        } while (ret != Z_STREAM_END);

        flush_buffer();
        deflateEnd(&strm);

    // --- DECOMPRESSION ---
    } else {
        if (inflateInit(&strm) != Z_OK) {
        	throw std::runtime_error("zlib: inflateInit failed");
        }

        strm.next_in = data_vec.data();
        std::size_t input_left = input_size;
        strm.avail_in = 0;

        int ret;
        while (true) {
            if (strm.avail_in == 0 && input_left > 0) {
                const std::size_t chunk = std::min(input_left, static_cast<std::size_t>(std::numeric_limits<uInt>::max()));
                strm.avail_in = static_cast<uInt>(chunk);
                input_left -= chunk;
            }

            if (strm.avail_out == 0) {
                flush_buffer();
            }

            ret = inflate(&strm, input_left > 0 ? Z_NO_FLUSH : Z_FINISH);

            if (ret == Z_STREAM_END) {
                flush_buffer();
                break;
            }
            if (ret == Z_BUF_ERROR) {
                if (strm.avail_out == 0) flush_buffer();
                continue;
            }
            if (ret != Z_OK) {
                std::string msg = strm.msg ? strm.msg : "code " + std::to_string(ret);
                inflateEnd(&strm);
                throw std::runtime_error("zlib inflate error: " + msg);
            }
        }
        inflateEnd(&strm);
    }
    data_vec = std::move(output_vec);
}

static vBytes readFile(const fs::path& path, FileTypeCheck FileType = FileTypeCheck::data_file) {	
	if (!fs::exists(path.string()) || !fs::is_regular_file(path.string())) {
    	throw std::runtime_error("Error: File \"" + path.string() + "\" not found or not a regular file.");
    }

	std::size_t file_size = fs::file_size(path.string());

	if (!file_size) {
		throw std::runtime_error("Error: File is empty.");
    }
    	
    if (FileType == FileTypeCheck::cover_image || FileType == FileTypeCheck::embedded_image) {
    	// Some platform apps, such as Bluesky mobile and X-Twitter mobile, will often save .jpg images with a .png extension, even though the content/file type is still JPEG.
    	// That is why I allow for the .png extension here, although the content must still be JPEG, as the file will be checked for the correct format later.
    	if (!hasFileExtension(path.string(), {".png", ".jpg", ".jpeg", ".jfif"})) {
        	throw std::runtime_error("File Type Error: Invalid image extension. Only expecting \".jpg\", \".jpeg\", \".jfif\" or \".png\".");
    	}
    		
    	if (FileType == FileTypeCheck::cover_image) {
			constexpr std::size_t MINIMUM_IMAGE_SIZE = 134ULL;
			
    		if (MINIMUM_IMAGE_SIZE > file_size) {
        		throw std::runtime_error("File Error: Invalid image file size.");
    		}	
			
			constexpr std::size_t MAX_IMAGE_SIZE = 8ULL * 1024 * 1024; // 8 MB.
		
			if (file_size > MAX_IMAGE_SIZE) {
				throw std::runtime_error("Image File Error: Cover image file exceeds maximum size limit.");
			}
    	}
    }
    	
    // Initially allow a data file (conceal mode) or image file (recover mode) up to 3GB. 
    // Will we impose smaller limits later, after compression and depeding on file type (image or data file), mode and platform options.
	constexpr std::size_t MAX_FILE_SIZE = 3ULL * 1024 * 1024 * 1024; // 3GB.
	
    if (file_size > MAX_FILE_SIZE) {
    	throw std::runtime_error("Error: File exceeds program size limit.");
    }

	if (!hasValidFilename(path.string())) {
		throw std::runtime_error("Invalid Input Error: Unsupported characters in filename arguments.");
    }

	std::ifstream file(path.string(), std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + path.string());
    }
    vBytes vec(file_size);

    file.read(reinterpret_cast<char*>(vec.data()), static_cast<std::streamsize>(file_size));

    if (file.gcount() != static_cast<std::streamsize>(file_size)) {
        throw std::runtime_error("Failed to read full file: partial read");
    }
    return vec;
}

static int concealData(vBytes& jpg_vec, Mode mode, Option option, fs::path& data_file_path) {    
	vString platforms_vec { 
    	"X-Twitter", "Tumblr", 
        "Bluesky. (Only share this \"file-embedded\" JPG image on Bluesky).\n\n You must use the Python script \"bsky_post.py\" (found in the repo src folder)\n to post the image to Bluesky.", 
        "Mastodon", "Pixelfed", "Reddit. (Only share this \"file-embedded\" JPG image on Reddit).",
        "PostImage", "ImgBB", "ImgPile",  "Flickr" 
    };
            
    bool 
    	isCompressedFile = false,
        hasNoOption      = (option == Option::None),
        hasBlueskyOption = (option == Option::Bluesky),
        hasRedditOption  = (option == Option::Reddit);
             
    optimizeImage(jpg_vec);
	
    constexpr size_t DQT_SEARCH_LIMIT = 100ULL;   
          
    constexpr auto 
    	DQT1_SIG = std::to_array<Byte>({ 0xFF, 0xDB, 0x00, 0x43 }),    
        DQT2_SIG = std::to_array<Byte>({ 0xFF, 0xDB, 0x00, 0x84 });
                
    auto 
    	dqt1 = searchSig(jpg_vec, std::span<const Byte>(DQT1_SIG), DQT_SEARCH_LIMIT),
        dqt2 = searchSig(jpg_vec, std::span<const Byte>(DQT2_SIG), DQT_SEARCH_LIMIT);

    if (!dqt1 && !dqt2) {
    	throw std::runtime_error("Image File Error: No DQT segment found (corrupt or unsupported JPG).");
    }

    const std::size_t NPOS = static_cast<std::size_t>(-1);
            
    std::size_t dqt_pos = std::min(dqt1.value_or(NPOS), dqt2.value_or(NPOS));
            
    // Erase everything before DQT.
    // This leaves the cover image with NO Start of Image (SOI) marker. 
    // We will write this back later... 
	
    jpg_vec.erase(jpg_vec.begin(), jpg_vec.begin() + static_cast<std::ptrdiff_t>(dqt_pos));

    std::size_t jpg_size = jpg_vec.size(); 
    
    constexpr std::size_t
    	MAX_OPTIMIZED_IMAGE_SIZE    = 4ULL 	  * 1024 * 1024,    
    	MAX_OPTIMIZED_BLUESKY_IMAGE = 805ULL  * 1024,
    	DATA_FILENAME_MAX_LENGTH    = 20ULL;           
        
    if (jpg_size > MAX_OPTIMIZED_IMAGE_SIZE) {
    	throw std::runtime_error("Image File Error: Cover image file exceeds maximum size limit.");
    }
            
    if (hasBlueskyOption && jpg_size > MAX_OPTIMIZED_BLUESKY_IMAGE) {
    	throw std::runtime_error("File Size Error: Image file exceeds maximum size limit for the Bluesky platform.");
    }
            
    vBytes data_vec = readFile(data_file_path, FileTypeCheck::data_file);
    std::size_t data_size = data_vec.size();
    
    std::string data_filename = data_file_path.filename().string();

    if (data_filename.size() > DATA_FILENAME_MAX_LENGTH) {
    	throw std::runtime_error("Data File Error: For compatibility requirements, length of data filename must not exceed 20 characters.");
    }
	
  	// Is data file greater than 10MB and matches one of these file extensions?
    isCompressedFile = data_size > 10ULL * 1024 * 1024 && hasFileExtension(data_file_path, {".zip",".jar",".rar",".7z",".bz2",".gz",".xz",".tar",".lz",".lz4",".cab",".rpm",".deb", ".mp4",".mp3",".exe",".jpg",".jpeg",".jfif",".png",".webp",".bmp",".gif",".ogg",".flac"});
                                                
  	// ICC color profile segment (FFE2). Default method for storing data file (in multiple segments, if required).
	// Notes: 	Total segments value index = 0x2E0 (2 bytes)
	//			Compressed data file size index = 0x2E2	(4 bytes)
	//			Data filename length index = 0x2E6 (1 byte)
	//			Data filename index = 0x2E7 (20 bytes)
	//			Data filename XOR key index = 0x2FB (24 bytes)
	//			Sodium key index = 0x313 (32 bytes)
	//			Nonce key index = 0x333 (24 bytes)
	//			jdvrif sig index = 0x34B (7 bytes)
	//			Data file start index = 0x353 (see index 0x2E2 (4 bytes) for compressed data file size).

	vBytes segment_vec {
		0xFF, 0xD8, 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0xEF,
		0x41, 0x44, 0x42, 0x45, 0x04, 0x20, 0x00, 0x00, 0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42, 0x20, 0x58, 0x59, 0x5A, 0x20, 0x07, 0xE5, 0x00, 0x04,
		0x00, 0x1B, 0x00, 0x0A, 0x00, 0x1B, 0x00, 0x00, 0x61, 0x63, 0x73, 0x70, 0x4D, 0x53, 0x46, 0x54, 0x00, 0x00, 0x00, 0x00, 0x73, 0x61, 0x77, 0x73,
		0x63, 0x74, 0x72, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF6, 0xD6, 0x00, 0x01, 0x00, 0x00,
		0x00, 0x00, 0xD3, 0x2D, 0x68, 0x61, 0x6E, 0x64, 0x40, 0x92, 0xFF, 0x1E, 0x67, 0x34, 0xB5, 0x6D, 0x00, 0x1C, 0x4E, 0x36, 0x73, 0x3F, 0x4E, 0x71,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x64, 0x65, 0x73, 0x63, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x00, 0x00, 0x24, 0x63, 0x70, 0x72, 0x74,
		0x00, 0x00, 0x01, 0x20, 0x00, 0x00, 0x00, 0x22, 0x77, 0x74, 0x70, 0x74, 0x00, 0x00, 0x01, 0x44, 0x00, 0x00, 0x00, 0x14, 0x63, 0x68, 0x61, 0x64,
		0x00, 0x00, 0x01, 0x58, 0x00, 0x00, 0x00, 0x2C, 0x72, 0x58, 0x59, 0x5A, 0x00, 0x00, 0x01, 0x84, 0x00, 0x00, 0x00, 0x14, 0x67, 0x58, 0x59, 0x5A,
		0x00, 0x00, 0x01, 0x98, 0x00, 0x00, 0x00, 0x14, 0x62, 0x58, 0x59, 0x5A, 0x00, 0x00, 0x01, 0xAC, 0x00, 0x00, 0x00, 0x14, 0x72, 0x54, 0x52, 0x43,
		0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x00, 0x20, 0x67, 0x54, 0x52, 0x43, 0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x00, 0x20, 0x62, 0x54, 0x52, 0x43,
		0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x00, 0x20, 0x6D, 0x6C, 0x75, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0C,
		0x65, 0x6E, 0x55, 0x53, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x41, 0x00, 0x39, 0x00, 0x38, 0x00, 0x43, 0x6D, 0x6C, 0x75, 0x63,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x65, 0x6E, 0x55, 0x53, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1C,
		0x00, 0x43, 0x00, 0x43, 0x00, 0x30, 0x00, 0x00, 0x58, 0x59, 0x5A, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF6, 0xD6, 0x00, 0x01, 0x00, 0x00,
		0x00, 0x00, 0xD3, 0x2D, 0x73, 0x66, 0x33, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0C, 0x42, 0x00, 0x00, 0x05, 0xDE, 0xFF, 0xFF, 0xF3, 0x25,
		0x00, 0x00, 0x07, 0x93, 0x00, 0x00, 0xFD, 0x90, 0xFF, 0xFF, 0xFB, 0xA1, 0xFF, 0xFF, 0xFD, 0xA2, 0x00, 0x00, 0x03, 0xDC, 0x00, 0x00, 0xC0, 0x6E,
		0x58, 0x59, 0x5A, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x18, 0x00, 0x00, 0x4F, 0xA5, 0x00, 0x00, 0x04, 0xFC, 0x58, 0x59, 0x5A, 0x20,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x8D, 0x00, 0x00, 0xA0, 0x2C, 0x00, 0x00, 0x0F, 0x95, 0x58, 0x59, 0x5A, 0x20, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x26, 0x31, 0x00, 0x00, 0x10, 0x2F, 0x00, 0x00, 0xBE, 0x9C, 0x70, 0x61, 0x72, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
		0x00, 0x02, 0x33, 0x33, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x0E, 0x41, 0xFF, 0xDB, 0x00, 0x43,
		0x00, 0x05, 0x03, 0x04, 0x04, 0x04, 0x03, 0x05, 0x04, 0x04, 0x04, 0x05, 0x05, 0x05, 0x06, 0x07, 0x0C, 0x08, 0x07, 0x07, 0x07, 0x07, 0x0F, 0x0B,
		0x0B, 0x09, 0x0C, 0x11, 0x0F, 0x12, 0x12, 0x11, 0x0F, 0x11, 0x11, 0x13, 0x16, 0x1C, 0x17, 0x13, 0x14, 0x1A, 0x15, 0x11, 0x11, 0x18, 0x21, 0x18,
		0x1A, 0x1D, 0x1D, 0x1F, 0x1F, 0x1F, 0x13, 0x17, 0x22, 0x24, 0x22, 0x1E, 0x24, 0x1C, 0x1E, 0x1F, 0x1E, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x05, 0x05,
		0x05, 0x07, 0x06, 0x07, 0x0E, 0x08, 0x08, 0x0E, 0x1E, 0x14, 0x11, 0x14, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E,
		0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E,
		0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0xFF, 0xC2, 0x00, 0x11, 0x08, 0x04, 0x00, 0x04, 0x00, 0x03,
		0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF, 0xC4, 0x00, 0x1C, 0x00, 0x00, 0x02, 0x03, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x08, 0xFF, 0xC4, 0x00, 0x1B, 0x01, 0x00, 0x03, 0x01, 0x01,
		0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0xFF, 0xDA, 0x00, 0x0C,
		0x03, 0x01, 0x00, 0x02, 0x10, 0x03, 0x10, 0x00, 0x00, 0x01, 0xF0, 0x43, 0x6B, 0x20, 0x42, 0xC4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCA,
		0xFD, 0x64, 0xC2, 0x21, 0x63, 0x7B, 0x37, 0xE5, 0x4E, 0xEC, 0xC7, 0xDE, 0x2B, 0x97, 0x48, 0x8C, 0x7A, 0x24, 0x65, 0x88, 0x94, 0x10, 0xB6, 0x44,
		0x11, 0x24, 0x7B, 0x80, 0x3D, 0x9F, 0xA8, 0xB0, 0x05, 0xE3, 0x30, 0xF8, 0xFD, 0xEC, 0x50, 0xF9, 0x9B, 0xDD, 0x9E, 0xB6, 0xFE, 0xBC, 0x93, 0xAD,
		0xE1, 0xFD, 0x39, 0xB2, 0x7F, 0x6F, 0x74, 0xAB, 0x7E, 0x4B, 0x36, 0xFC, 0xF2, 0xA8, 0x2B, 0xD0, 0x00, 0x04, 0x28, 0x43, 0xF3, 0x0A, 0xEF, 0x76,
		0xEE, 0xAC, 0x08, 0x71, 0xBB, 0xAA, 0x77, 0xB7, 0xB1, 0x50, 0x0F, 0xB1, 0x9B, 0x34, 0xB3, 0x29, 0x12, 0x15, 0x51, 0x64, 0x85, 0xF7, 0x91, 0x9A,
		0xFD, 0xD5, 0x82, 0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9, 0x90	
	};

	// EXIF (FFE1) segment. This is the way we store the data file when user selects the -b option switch for Bluesky platform. 
	// Notes: 	Total segments value index = N/A
	//			Compressed data file size index = 0x1CD	(4 bytes)
	//			Data filename length index = 0x160 (1 byte)
	//			Data filename index = 0x161 (20 bytes)
	//			Data filename XOR key index = 0x175 (24 bytes)
	//			Sodium key index = 0x18D (32 bytes)
	//			Nonce key index = 0x1AD (24 bytes)
	//			jdvrif sig index = 0x1C5 (7 bytes)
	//			Data file start index = 0x1D1 (see index 0x1CD (4 bytes) for compressed data file size).

	vBytes bluesky_exif_vec {
		0xFF, 0xD8, 0xFF, 0xE1, 0xFF, 0xFE, 0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x4D, 0x4D, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x06, 0x01, 0x12,
		0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x1A, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x1B,
		0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x28, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x01, 0x3B,
		0x00, 0x02, 0x00, 0x00, 0xFF, 0x72, 0x00, 0x00, 0x00, 0x56, 0x87, 0x69, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x03, 0x02, 0x02, 0x03, 0x02, 0x02, 0x03, 0x03, 0x03, 0x03, 0x04, 0x03, 0x03, 0x04, 0x05, 0x08, 0x05,
		0x05, 0x04, 0x04, 0x05, 0x0A, 0x07, 0x07, 0x06, 0x08, 0x0C, 0x0A, 0x0C, 0x0C, 0x0B, 0x0A, 0x0B, 0x0B, 0x0D, 0x0E, 0x12, 0x10, 0x0D, 0x0E, 0x11,
		0x0E, 0x0B, 0x0B, 0x10, 0x16, 0x10, 0x11, 0x13, 0x14, 0x15, 0x15, 0x15, 0x0C, 0x0F, 0x17, 0x18, 0x16, 0x14, 0x18, 0x12, 0x14, 0x15, 0x14, 0xFF,
		0xDB, 0x00, 0x43, 0x01, 0x03, 0x04, 0x04, 0x05, 0x04, 0x05, 0x09, 0x05, 0x05, 0x09, 0x14, 0x0D, 0x0B, 0x0D, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
		0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
		0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0xFF, 0xC0, 0x00, 0x11,
		0x08, 0x06, 0x0C, 0x07, 0x80, 0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF, 0xC4, 0x00, 0x1C, 0x00, 0x00, 0x02, 0x03, 0x01,
		0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x04, 0x01, 0x02, 0x05, 0x00, 0x06, 0x07, 0x08, 0xFF, 0xC4, 0x00,
		0x1B, 0x01, 0x00, 0x02, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00, 0x04, 0x05, 0x01,
		0x06, 0x07, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x10, 0x03, 0x10, 0x00, 0x00, 0x01, 0xF8, 0x29, 0xC6, 0x7A, 0x1F, 0x49, 0x1A, 0x8E,
		0xAB, 0x38, 0xA0, 0x8C, 0x26, 0x63, 0x8E, 0x97, 0x83, 0xA9, 0xD3, 0xD7, 0x12, 0xEB, 0x75, 0xF8, 0x00, 0x6E, 0x96, 0xF9, 0xB0, 0x46, 0xDE, 0x8B,
		0x7A, 0x82, 0x60, 0x43, 0xAD, 0xA0, 0x62, 0x68, 0xCC, 0xD0, 0xA8, 0xA1, 0x0D, 0x01, 0x59, 0x85, 0xE1, 0xA3, 0x52, 0x57, 0x3A, 0x92, 0x41, 0x84,
		0x5C, 0x3C, 0xF4, 0xFF, 0x82, 0xCA, 0x30, 0x79, 0x0E, 0x97, 0xA0, 0x36, 0xB0, 0x7D, 0x72, 0x92, 0x6A, 0x31, 0xD0, 0x09, 0x0A, 0x77, 0x90, 0xA7,
		0x36, 0x66, 0xA0, 0x26, 0xAB, 0xBC, 0xDF, 0x61, 0xFE, 0xEE, 0xD9, 0x46, 0x70, 0xD7, 0xB0, 0x79, 0xF5, 0xA5, 0x29, 0xD8, 0xAB, 0x1F, 0x58, 0xE2,
		0xAF, 0xA8, 0x1F, 0x00, 0xDB, 0x32, 0x1F, 0xC2, 0xFD, 0x55, 0x21, 0x27, 0x3A, 0x6A, 0xBE, 0x1D, 0xB8, 0x5F, 0x60, 0x38, 0x99, 0xB4, 0x6A, 0x3E,
		0xEA, 0x5E, 0x9D, 0xF9, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x02, 0xA0, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x06, 0x00, 0xA0, 0x03, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00,
		0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00
	};
						
	if (hasBlueskyOption) {
		// Use the EXIF segment instead of the default color profile segment to store user data.
		// The color profile segment (FFE2) is removed by Bluesky, so we use EXIF.
		segment_vec = std::move(bluesky_exif_vec);
	} 
	vBytes().swap(bluesky_exif_vec);
            
    const std::size_t DATA_FILENAME_LENGTH_INDEX = hasBlueskyOption ? 0x160 : 0x2E6;

    segment_vec[DATA_FILENAME_LENGTH_INDEX] = static_cast<Byte>(data_filename.size());     
        
    constexpr std::size_t LARGE_FILE_SIZE = 300ULL * 1024 * 1024; // 300 MB
               
    if (data_size > LARGE_FILE_SIZE) {
        std::cout << "\nPlease wait. Larger files will take longer to complete this process.\n";    
    }
            
    if (isCompressedFile) {
    	// Data file meets criteria for "already compressed", so skip zlib function and mark cover image so zlib will also be skipped during recovery.
    	segment_vec[NO_ZLIB_COMPRESSION_ID_INDEX] = NO_ZLIB_COMPRESSION_ID; 
    } else {
    	zlibFunc(data_vec, mode);
    	data_size = data_vec.size(); 
    }
   
    constexpr std::size_t 
    	MAX_SIZE_CONCEAL      = 2ULL  * 1024 * 1024 * 1024,  // 2GB.
        MAX_SIZE_REDDIT       = 20ULL * 1024 * 1024,         // 20MB.
        MAX_DATA_SIZE_BLUESKY = 2ULL  * 1024 * 1024;         // 2MB. 
                
    const std::size_t COMBINED_FILE_SIZE = data_size + jpg_size;
                    
    if (hasBlueskyOption && data_size > MAX_DATA_SIZE_BLUESKY) {
        throw std::runtime_error("Data File Size Error: File exceeds maximum size limit for the Bluesky platform.");
    }

    if (hasRedditOption && COMBINED_FILE_SIZE > MAX_SIZE_REDDIT) {
        throw std::runtime_error("File Size Error: Combined size of image and data file exceeds maximum size limit for the Reddit platform.");
    }

    if (hasNoOption && COMBINED_FILE_SIZE > MAX_SIZE_CONCEAL) {
        throw std::runtime_error("File Size Error: Combined size of image and data file exceeds maximum default size limit for jdvrif.");
    }
    
    std::size_t recovery_pin = encryptDataFile(segment_vec, data_vec, jpg_vec, platforms_vec, data_filename, hasBlueskyOption, hasRedditOption);
    
    const uint32_t RAND_NUM = 100000 + randombytes_uniform(900000);
    const std::string OUTPUT_FILENAME = "jrif_" + std::to_string(RAND_NUM) + ".jpg";
    
    std::ofstream file_ofs(OUTPUT_FILENAME, std::ios::binary);

    if (!file_ofs) {
        throw std::runtime_error("Write File Error: Unable to write to file. Make sure you have WRITE permissions for this location.");
    }
    
    // Better performance to write separately a vector containing a large data file, 
    // instead of inserting content of one vector into the large file vector, followed by writing out the vector/file.
    // If segment_vec is large (>10MB), write it first, followed by writing jpg_vec.
    // If segment_vec is empty (Bluesky/Reddit) or small, this seperate write is skipped, as everthing 
    // has already been inserted into jpg_vec.
	
    if (!segment_vec.empty()) {
    	file_ofs.write(reinterpret_cast<const char*>(segment_vec.data()), segment_vec.size());
    }
	
    file_ofs.write(reinterpret_cast<const char*>(jpg_vec.data()), jpg_vec.size());
    file_ofs.close();
    
    const std::size_t EMBEDDED_JPG_SIZE = segment_vec.size() + jpg_vec.size();
    
    if (hasNoOption) {
    	constexpr std::size_t 
        	FLICKR_MAX_IMAGE_SIZE          = 200ULL * 1024 * 1024,
            IMGPILE_MAX_IMAGE_SIZE         = 100ULL * 1024 * 1024,
            IMGBB_POSTIMAGE_MAX_IMAGE_SIZE = 32ULL  * 1024 * 1024,
            MASTODON_MAX_IMAGE_SIZE        = 16ULL  * 1024 * 1024,
            PIXELFED_MAX_IMAGE_SIZE        = 15ULL  * 1024 * 1024,
            TWITTER_MAX_IMAGE_SIZE         = 5ULL   * 1024 * 1024,
            TWITTER_MAX_DATA_SIZE          = 10ULL  * 1024,    
            TUMBLR_MAX_DATA_SIZE           = 65534ULL,
            BYTE_LENGTH		   	   		   = 2ULL,
            TOTAL_SEGMENTS_INDEX  	  	   = 0x2E0ULL,
            FIRST_SEGMENT_SIZE_INDEX	   = 0x04ULL;
           
        constexpr uint16_t MASTODON_MAX_SEGMENTS = 100;
              
        const std::span<const Byte> VEC = segment_vec.empty()
    		? std::span(jpg_vec)
    		: std::span(segment_vec);
        
        const uint16_t
            FIRST_SEGMENT_SIZE = static_cast<uint16_t>(getValue(view(VEC), FIRST_SEGMENT_SIZE_INDEX, BYTE_LENGTH)),
            TOTAL_SEGMENTS     = static_cast<uint16_t>(getValue(view(VEC), TOTAL_SEGMENTS_INDEX, BYTE_LENGTH));
        
        vBytes().swap(segment_vec);
        vBytes().swap(jpg_vec);
        
		std::erase_if(platforms_vec, [&](const std::string& platform) {
    		if (platform == "X-Twitter" && (FIRST_SEGMENT_SIZE > TWITTER_MAX_DATA_SIZE || EMBEDDED_JPG_SIZE > TWITTER_MAX_IMAGE_SIZE)) {
        		return true; 
    		}
    		if (platform == "Tumblr" && FIRST_SEGMENT_SIZE > TUMBLR_MAX_DATA_SIZE) {
        		return true; 
    		}
    		if (platform == "Mastodon" && (TOTAL_SEGMENTS > MASTODON_MAX_SEGMENTS || EMBEDDED_JPG_SIZE > MASTODON_MAX_IMAGE_SIZE)) {
        		return true;
    		}
    		if (platform == "Pixelfed" && EMBEDDED_JPG_SIZE > PIXELFED_MAX_IMAGE_SIZE) {
        		return true;
    		}
    		if ((platform == "ImgBB" || platform == "PostImage") && EMBEDDED_JPG_SIZE > IMGBB_POSTIMAGE_MAX_IMAGE_SIZE) {
        		return true;
    		}
    		if (platform == "ImgPile" && EMBEDDED_JPG_SIZE > IMGPILE_MAX_IMAGE_SIZE) {
        		return true;
    		}
    		if (platform == "Flickr" && EMBEDDED_JPG_SIZE > FLICKR_MAX_IMAGE_SIZE) {
       			return true;
    		}	
    		return false; 
		});

		if (platforms_vec.empty()) {
    		platforms_vec.emplace_back("\b\bUnknown!\n\n Due to the large file size of the output JPG image, I'm unaware of any\n compatible platforms that this image can be posted on. Local use only?");
		}
	}
	std::cout << "\nPlatform compatibility for output image:-\n\n";
	for (const auto& s : platforms_vec) {
    	std::cout << " ✓ " << s << '\n';
	}        
    std::cout << "\nSaved \"file-embedded\" JPG image: " << OUTPUT_FILENAME  << " (" << EMBEDDED_JPG_SIZE << " bytes).\n";
    std::cout << "\nRecovery PIN: [***" << recovery_pin << "***]\n\nImportant: Keep your PIN safe, so that you can extract the hidden file.\n\nComplete!\n\n";
    return 0;
}

static int recoverData(vBytes& jpg_vec, Mode mode, fs::path& image_file_path) {
	constexpr std::size_t 
		SIG_LENGTH = 7ULL,
		INDEX_DIFF = 8ULL;

	constexpr auto 
		JDVRIF_SIG 		= std::to_array<Byte>({ 0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9 }),
		ICC_PROFILE_SIG = std::to_array<Byte>({ 0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42 });
	
	auto index_opt = searchSig(jpg_vec, std::span<const Byte>(JDVRIF_SIG));
				
	if (!index_opt) {
		throw std::runtime_error("Image File Error: Signature check failure. This is not a valid jdvrif \"file-embedded\" image.");
	}
			
	const std::size_t JDVRIF_SIG_INDEX = *index_opt;
	
	Byte pin_attempts_val = jpg_vec[JDVRIF_SIG_INDEX + INDEX_DIFF - 1];
			
	bool 
		isBlueskyFile 	 = true,
		isDataCompressed = true;
			
	index_opt = searchSig(jpg_vec, std::span<const Byte>(ICC_PROFILE_SIG));
	
	if (index_opt) {
		constexpr std::size_t NO_ZLIB_COMPRESSION_ID_INDEX_DIFF = 24ULL;
		const std::size_t ICC_PROFILE_SIG_INDEX = *index_opt;	
				
		jpg_vec.erase(jpg_vec.begin(), jpg_vec.begin() + (ICC_PROFILE_SIG_INDEX - INDEX_DIFF));	
		isDataCompressed = (jpg_vec[NO_ZLIB_COMPRESSION_ID_INDEX - NO_ZLIB_COMPRESSION_ID_INDEX_DIFF] != NO_ZLIB_COMPRESSION_ID);	
		isBlueskyFile = false;
	}
	
	if (isBlueskyFile) { // EXIF segment (FFE1) for data file storage is used instead of ICC Profile (FFE2) segment. Also check for PHOTOSHOP & XMP segments and their index locations.
		constexpr size_t SEARCH_LIMIT = 125480ULL; 
    	constexpr auto 
			PSHOP_SEGMENT_SIG = std::to_array<Byte>({ 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E }),
			XMP_CREATOR_SIG   = std::to_array<Byte>({ 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69 });

    	index_opt = searchSig(jpg_vec, std::span<const Byte>(PSHOP_SEGMENT_SIG), SEARCH_LIMIT);
    		
    	if (index_opt) {
    		// Found Photoshop segment.
        	constexpr std::size_t 
        		DATASET_MAX_SIZE 	      	  = 32800ULL, // If the photoshop segment size is greater than this size, we have two datasets (max).
            	PSHOP_SEGMENT_SIZE_INDEX_DIFF = 7ULL,
            	FIRST_DATASET_SIZE_INDEX_DIFF = 24ULL,
            	DATASET_FILE_INDEX_DIFF       = 2ULL,
            	BYTE_LENGTH 		      	  = 2ULL;

        	const std::size_t
            	PSHOP_SEGMENT_SIG_INDEX  = *index_opt,
            	PSHOP_SEGMENT_SIZE_INDEX = PSHOP_SEGMENT_SIG_INDEX  - PSHOP_SEGMENT_SIZE_INDEX_DIFF,
            	FIRST_DATASET_SIZE_INDEX = PSHOP_SEGMENT_SIG_INDEX  + FIRST_DATASET_SIZE_INDEX_DIFF,
            	FIRST_DATASET_FILE_INDEX = FIRST_DATASET_SIZE_INDEX + DATASET_FILE_INDEX_DIFF;

        	const uint16_t
            	PSHOP_SEGMENT_SIZE = static_cast<uint16_t>(getValue(view(jpg_vec), PSHOP_SEGMENT_SIZE_INDEX, BYTE_LENGTH)),
            	FIRST_DATASET_SIZE = static_cast<uint16_t>(getValue(view(jpg_vec), FIRST_DATASET_SIZE_INDEX, BYTE_LENGTH));

			vBytes file_parts_vec; 

            file_parts_vec.reserve(FIRST_DATASET_SIZE * 5ULL);	
            file_parts_vec.insert(file_parts_vec.end(), jpg_vec.begin() + FIRST_DATASET_FILE_INDEX, jpg_vec.begin() + FIRST_DATASET_FILE_INDEX + FIRST_DATASET_SIZE);
            		
            bool hasXmpSegment = false;
            std::size_t xmp_creator_sig_index = 0;
            	
        	if (PSHOP_SEGMENT_SIZE > DATASET_MAX_SIZE) {
            	constexpr std::size_t SECOND_DATASET_SIZE_INDEX_DIFF = 3ULL;		
            	const std::size_t
                	SECOND_DATASET_SIZE_INDEX = FIRST_DATASET_FILE_INDEX  + FIRST_DATASET_SIZE + SECOND_DATASET_SIZE_INDEX_DIFF,
                	SECOND_DATASET_FILE_INDEX = SECOND_DATASET_SIZE_INDEX + DATASET_FILE_INDEX_DIFF;

            	const uint16_t SECOND_DATASET_SIZE = static_cast<uint16_t>(getValue(view(jpg_vec), SECOND_DATASET_SIZE_INDEX, BYTE_LENGTH));
            	
            	file_parts_vec.insert(file_parts_vec.end(), jpg_vec.begin() + SECOND_DATASET_FILE_INDEX, jpg_vec.begin() + SECOND_DATASET_FILE_INDEX + SECOND_DATASET_SIZE);

				// Now check to see if we have an XMP segment. (Always base64 data).
            	index_opt = searchSig(jpg_vec, std::span<const Byte>(XMP_CREATOR_SIG), SEARCH_LIMIT);
            	if (index_opt) {
                	hasXmpSegment = true;
    				xmp_creator_sig_index = *index_opt;
					
                	constexpr Byte BASE64_END_SIG = 0x3C;
					
                    const std::size_t 
						BASE64_BEGIN_INDEX 	 = xmp_creator_sig_index + SIG_LENGTH + 1,
                		BASE64_END_SIG_INDEX = static_cast<std::size_t>(std::find(jpg_vec.begin() + BASE64_BEGIN_INDEX, jpg_vec.end(), BASE64_END_SIG) - jpg_vec.begin()),
                		BASE64_SIZE 	     = BASE64_END_SIG_INDEX - BASE64_BEGIN_INDEX;
                			
					std::span<const Byte> base64_span(jpg_vec.data() + BASE64_BEGIN_INDEX, BASE64_SIZE);
					appendBase64AsBinary(base64_span, file_parts_vec);
            	}
        	}
        	const std::size_t 
        		EXIF_DATA_END_INDEX_DIFF = hasXmpSegment  ? 351 : 55,
       			EXIF_DATA_END_INDEX 	 = (hasXmpSegment ? xmp_creator_sig_index : PSHOP_SEGMENT_SIG_INDEX) - EXIF_DATA_END_INDEX_DIFF;
       		
        		std::copy_n(file_parts_vec.begin(), file_parts_vec.size(), jpg_vec.begin() + EXIF_DATA_END_INDEX);
    		}
	}			
	bool hasDecryptionFailed = false;
			
	std::string decrypted_filename = decryptDataFile(jpg_vec, isBlueskyFile, hasDecryptionFailed);
			
	std::streampos pin_attempts_index = JDVRIF_SIG_INDEX + INDEX_DIFF - 1;
			 
	if (hasDecryptionFailed) {	
		std::fstream file(image_file_path, std::ios::in | std::ios::out | std::ios::binary);
		
		if (pin_attempts_val == 0x90) {
			pin_attempts_val = 0;
		} else {
    		pin_attempts_val++;
		}
		if (pin_attempts_val > 2) {
			file.close();
			std::ofstream file(image_file_path, std::ios::out | std::ios::trunc | std::ios::binary);
		} else {
			file.seekp(pin_attempts_index);
			file.write(reinterpret_cast<char*>(&pin_attempts_val), sizeof(pin_attempts_val));
		}
		file.close();
		throw std::runtime_error("File Decryption Error: Invalid recovery PIN or file is corrupt.");
	}
	// Inflate data file with Zlib.
	if (isDataCompressed) {
		zlibFunc(jpg_vec, mode);
	}
		
	const std::size_t INFLATED_FILE_SIZE = jpg_vec.size();
			
	if (!INFLATED_FILE_SIZE) {
		throw std::runtime_error("Zlib Compression Error: Output file is empty. Inflating file failed.");
	}

	if (pin_attempts_val != 0x90) {
		std::fstream file(image_file_path, std::ios::in | std::ios::out | std::ios::binary);
		
		Byte reset_pin_attempts_val = 0x90;

		file.seekp(pin_attempts_index);
		file.write(reinterpret_cast<char*>(&reset_pin_attempts_val), sizeof(reset_pin_attempts_val));

		file.close();
	}

	std::ofstream file_ofs(decrypted_filename, std::ios::binary);

	if (!file_ofs) {
		throw std::runtime_error("Write Error: Unable to write to file. Make sure you have WRITE permissions for this location.");
	}

	file_ofs.write(reinterpret_cast<const char*>(jpg_vec.data()), INFLATED_FILE_SIZE);
	file_ofs.close();
		
	std::cout << "\nExtracted hidden file: " << decrypted_filename << " (" << INFLATED_FILE_SIZE << " bytes).\n\nComplete! Please check your file.\n\n";
	return 0;		
}

int main(int argc, char** argv) {
	try {
		if (sodium_init() < 0) {             
       		throw std::runtime_error("Libsodium initialization failed!");
    	}
    		
		#ifdef _WIN32
    		SetConsoleOutputCP(CP_UTF8);  
		#endif
		
		auto args_opt = ProgramArgs::parse(argc, argv);
       	if (!args_opt) return 0; 
       		
		ProgramArgs args = *args_opt; 
		
		bool isConcealMode = (args.mode == Mode::conceal);
    	
		vBytes jpg_vec = readFile(args.image_file_path, isConcealMode ? FileTypeCheck::cover_image : FileTypeCheck::embedded_image);
			
        if (isConcealMode) {                                    
			concealData(jpg_vec, args.mode, args.option, args.data_file_path);	
        } else {
        	recoverData(jpg_vec, args.mode, args.image_file_path);
        }
   	}
	catch (const std::runtime_error& e) {
    	std::cerr << "\n" << e.what() << "\n\n";
    	return 1;
    }
}
