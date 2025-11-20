// JPG Data Vehicle (jdvrif v6.5) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream> 
#include <iostream>
#include <initializer_list>
#include <iterator> 
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

using Key     = std::array<Byte, crypto_secretbox_KEYBYTES>;
using Nonce   = std::array<Byte, crypto_secretbox_NONCEBYTES>;
using Tag     = std::array<Byte, crypto_secretbox_MACBYTES>;

constexpr std::size_t TAG_BYTES = std::tuple_size<Tag>::value;

static void displayInfo() {
	std::cout << R"(

JPG Data Vehicle (jdvrif v6.5)
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
	• Pixelfed	(15 MB)

Limit measured by compressed data file size only:

	• Mastodon   (~6 MB)
	• Tumblr     (~64 KB)
	• X-Twitter  (~10 KB)

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

conceal - Compresses, encrypts and embeds your secret data file within a JPG cover image.
recover - Decrypts, uncompresses and extracts the concealed data file from a JPG cover image
          (recovery PIN required).

──────────────────────────
Platform options for conceal mode
──────────────────────────

-b (Bluesky) : Createa compatible “file-embedded” JPG images for posting on Bluesky.

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

enum class Mode   : unsigned char { conceal, recover };
enum class Option : unsigned char { None, Bluesky, Reddit };

struct ProgramArgs {
	Mode mode{Mode::conceal};
	Option option{Option::None};

	fs::path image_file_path;
	fs::path data_file_path;
    
	static std::optional<ProgramArgs> parse(int argc, char** argv) {
		using std::string_view;

        auto arg = [&](int i) -> string_view {
			return (i >= 0 && i < argc) ? string_view(argv[i]) : string_view{};
        };

        const std::string PROG = fs::path(argv[0]).filename().string(), USAGE = "Usage: " + PROG + " conceal [-b|-r] <cover_image> <secret_file>\n\t\b" + PROG + " recover <cover_image>\n\t\b" + PROG + " --info";
		
        auto die = [&]() -> void {
        	throw std::runtime_error(USAGE);
        };

        if (argc < 2) die();

        if (argc == 2 && arg(1) == "--info") {
        	displayInfo();
        	return std::nullopt;
        }

        ProgramArgs out{};

        const string_view MODE = arg(1);

        if (MODE == "conceal") {
        	Byte i = 2;
            if (arg(i) == "-b" || arg(i) == "-r") {
        		out.option = (arg(i) == "-b") ? Option::Bluesky : Option::Reddit;
            	++i;
            }
            if (i + 1 >= argc || (i + 2) != argc) die();

            out.image_file_path = fs::path(arg(i));
            out.data_file_path  = fs::path(arg(i + 1));
            out.mode = Mode::conceal;
            return out;
        }
        if (MODE == "recover") {
        	if (argc != 3) die();
        	out.image_file_path = fs::path(arg(2));
        	out.mode = Mode::recover;
        	return out;
        }
        die();
        return out; 
    }
};

// searchSig function searches a byte vector (uint8_t) for a fixed byte pattern and returns the offset of the first match, or std::nullopt if there’s no match.
// It uses std::search on v.begin().. v.end() with the pattern given by sig.begin().. sig.end(). 
	
// The std::span<const uint8_t> parameter lets you pass anything contiguous - std::array, C-array, another std::vector, or a subrange, without copying. 
// If std::search returns v.end(), the function maps that to std::nullopt; otherwise it converts the iterator difference to a size_t index.
static std::optional<std::size_t> searchSig(const vBytes& v, std::span<const Byte> sig) {
    constexpr std::size_t SCAN_LIMIT = 125380; 

    // 1. Optimization: If the file is large, try searching just the SCAN_LIMIT first.
    // Most signatures live here in that range.
    if (v.size() > SCAN_LIMIT) {
        auto it = std::search(v.begin(), v.begin() + SCAN_LIMIT, sig.begin(), sig.end());
        
        // If found within the limit, return immediately.
        if (it != v.begin() + SCAN_LIMIT) {
            return static_cast<std::size_t>(it - v.begin());
        }
    }

    // 2. Fallback: Search the whole file.
    // We reach here if the file is small or if the fast search failed.
    // We scan from the beginning again to ensure we catch signatures that might 
    // straddle the byte boundary (overlapping the cut-off).
    auto it = std::search(v.begin(), v.end(), sig.begin(), sig.end());
    
    if (it == v.end()) return std::nullopt;
    return static_cast<std::size_t>(it - v.begin());
}


// First search for an EXIF segment, if found search for an Orientation tag.
// Returns 1..8 if found and passed to getTransformOp, or std::nullopt if no EXIF/Orientation.
[[nodiscard]] static std::optional<uint16_t> exifOrientation(const vBytes& jpg) {
	constexpr const Byte APP1_SIG[] = {0xFF, 0xE1};
    auto app1_opt = searchSig(jpg, std::span<const Byte>(APP1_SIG, 2));
    	
    if (!app1_opt) return std::nullopt;

    std::size_t app1_pos = *app1_opt;
    	
    if (app1_pos + 4 > jpg.size()) return std::nullopt;

    uint16_t length = (static_cast<uint16_t>(jpg[app1_pos+2]) << 8) | jpg[app1_pos+3];
    std::size_t exif_end = app1_pos + 2 + length;            
    	
    if (exif_end > jpg.size()) return std::nullopt;

    std::size_t exif_start = app1_pos + 4;
    	
    if (exif_start + 6 > exif_end) return std::nullopt;
    if (std::memcmp(&jpg[exif_start], "Exif\0\0", 6) != 0) return std::nullopt;

    std::size_t tiff = exif_start + 6;
    	
    if (tiff + 8 > exif_end) return std::nullopt;

    bool isLittleEndian = false;
    	
    if (jpg[tiff] == 'I' && jpg[tiff+1] == 'I') isLittleEndian = true;
    	else if (jpg[tiff] == 'M' && jpg[tiff+1] == 'M') isLittleEndian = false;
    		else return std::nullopt;

    auto rd16 = [&](std::size_t off) -> uint16_t {
        if (off + 1 >= exif_end) return 0;
        return isLittleEndian ? (uint16_t)(jpg[off] | (jpg[off+1] << 8)) : (uint16_t)((jpg[off] << 8) | jpg[off+1]);
    };
    auto rd32 = [&](std::size_t off) -> uint32_t {
        if (off + 3 >= exif_end) return 0;
        return isLittleEndian ? (uint32_t)(jpg[off] | (jpg[off+1] << 8) | (jpg[off+2] << 16) | (jpg[off+3] << 24)) : (uint32_t)((jpg[off] << 24) | (jpg[off+1] << 16) | (jpg[off+2] << 8) | jpg[off+3]);
    };

    if (rd16(tiff + 2) != 0x002A) return std::nullopt;
    	
    uint32_t ifd0_off = rd32(tiff + 4);
    std::size_t ifd = tiff + ifd0_off;
    	
    if (ifd + 2 > exif_end) return std::nullopt;

    uint16_t count = rd16(ifd);
    	
    ifd += 2;
    	
    for (uint16_t i = 0; i < count; ++i) {
        std::size_t entry = ifd + i * 12;
        if (entry + 12 > exif_end) return std::nullopt;
        uint16_t tag = rd16(entry + 0);
        if (tag == 0x0112) {
        	return rd16(entry + 8); // 1..8 usually. 
        }
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

static void optimizeImage(vBytes& jpg_vec, int& width, int& height, bool hasNoOption) {
    if (jpg_vec.empty()) {
        throw std::runtime_error("JPG image is empty!");
    }

    TJHandle transformer;
    transformer.handle = tjInitTransform();
    if (!transformer.handle) {
        throw std::runtime_error("tjInitTransform() failed");
    }
  
    int jpegSubsamp = 0, jpegColorspace = 0;
    if (tjDecompressHeader3(transformer.get(), jpg_vec.data(), static_cast<unsigned long>(jpg_vec.size()), 
                           &width, &height, &jpegSubsamp, &jpegColorspace) != 0) {
        throw std::runtime_error(std::string("Image Error: ") + tjGetErrorStr2(transformer.get()));
    }

    // 2. Determine Rotation Operation from EXIF
    auto ori_opt = exifOrientation(jpg_vec);
    int xop = TJXOP_NONE;
    
    if (ori_opt) {
        xop = getTransformOp(*ori_opt);
    }

    tjtransform xform;
    std::memset(&xform, 0, sizeof(tjtransform));
    xform.op = xop;
   
    xform.options = TJXOPT_COPYNONE | TJXOPT_TRIM;

    // Toggle Progressive vs Baseline based on the flag
    if (hasNoOption) {
        xform.options |= TJXOPT_PROGRESSIVE;
    }
    // Else defaults to Baseline (which is what we want for Bluesky/Reddit)
	
    unsigned char* dstBuf = nullptr; 
    unsigned long dstSize = 0;

    if (tjTransform(transformer.get(), jpg_vec.data(), static_cast<unsigned long>(jpg_vec.size()), 1, &dstBuf, &dstSize, &xform, 0) != 0) {
         throw std::runtime_error(std::string("tjTransform: ") + tjGetErrorStr2(transformer.get()));
    }

    if (xop == TJXOP_ROT90 || xop == TJXOP_ROT270 || xop == TJXOP_TRANSPOSE || xop == TJXOP_TRANSVERSE) {
        std::swap(width, height);
    }
    jpg_vec.assign(dstBuf, dstBuf + dstSize);
    tjFree(dstBuf);
}

// Writes updated values (2, 4 or 8 bytes), such as segments lengths, index/offsets values, PIN, etc. into the relevant vector index location.	
static void updateValue(vBytes& vec, std::size_t index, std::size_t value, Byte bits) {
	while (bits > 0) {
		bits -= 8;
    	vec[index++] = static_cast<uint8_t>((value >> bits) & 0xFF);
    }
}

static std::size_t getValue(vBytes& vec, std::size_t index, std::size_t bytes) {
	if (bytes > 8 || 2 > bytes) {
    	throw std::out_of_range("getValue: Invalid bytes value. 2, 4 or 8 only.");
    }
    	
    if (index > vec.size() || vec.size() - index < bytes) {
    	throw std::out_of_range("getValue: Index out of bounds");
    }
    	
    std::size_t value = 0;
    	
    for (Byte i = 0; i < bytes; ++i) {
    	value = (value << 8) | static_cast<std::size_t>(vec[index + i]);
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

static void updateBlueskySegmentValues(vBytes& segment_vec, vBytes& pshop_vec, vBytes& xmp_vec, vBytes& jpg_vec, vString& platforms_vec) {
	constexpr Byte 
		MARKER_BYTES_SIZE 		  = 4, // FFD8, FFE1
		SIZE_FIELD_INDEX 		  = 0x04,  
		XRES_OFFSET_FIELD_INDEX   = 0x2A,  
		YRES_OFFSET_FIELD_INDEX   = 0x36,  
		ARTIST_SIZE_FIELD_INDEX   = 0x4A,  
		SUBIFD_OFFSET_FIELD_INDEX = 0x5A;  

	const std::size_t 
		EXIF_SEGMENT_SIZE = segment_vec.size() - MARKER_BYTES_SIZE,
		XRES_OFFSET   	  = EXIF_SEGMENT_SIZE - 0x36,
		YRES_OFFSET   	  = EXIF_SEGMENT_SIZE - 0x2E,
		SUBIFD_OFFSET 	  = EXIF_SEGMENT_SIZE - 0x26,
		ARTIST_SIZE   	  = EXIF_SEGMENT_SIZE - 0x8C;
		
	Byte value_bit_length = 16;
	
	updateValue(segment_vec, SIZE_FIELD_INDEX , EXIF_SEGMENT_SIZE, value_bit_length);
		
	value_bit_length = 32;

	updateValue(segment_vec, XRES_OFFSET_FIELD_INDEX, XRES_OFFSET, value_bit_length);
	updateValue(segment_vec, YRES_OFFSET_FIELD_INDEX, YRES_OFFSET, value_bit_length);
	updateValue(segment_vec, ARTIST_SIZE_FIELD_INDEX, ARTIST_SIZE, value_bit_length); 
	updateValue(segment_vec, SUBIFD_OFFSET_FIELD_INDEX, SUBIFD_OFFSET, value_bit_length);
					
	constexpr Byte PSHOP_VEC_DEFAULT_SIZE = 35;  // PSHOP segment size without user data.
					
	if (pshop_vec.size() > PSHOP_VEC_DEFAULT_SIZE) {
		// Data file was too big for the EXIF segment, so will spill over to the PSHOP vec.	
		constexpr Byte 
			SEGMENT_SIZE_INDEX 		  = 0x02,
			BIM_SIZE_INDEX 			  = 0x1C,
			BIM_SIZE_DIFF			  = 28,	// Consistant size difference between PSHOP segment size and BIM size.	
			SEGMENT_MARKER_BYTES_SIZE = 2;
						
		std::size_t pshop_segment_size = pshop_vec.size() - SEGMENT_MARKER_BYTES_SIZE;
						 
		value_bit_length = 16;	
						
		updateValue(pshop_vec, SEGMENT_SIZE_INDEX, pshop_segment_size, value_bit_length);
		updateValue(pshop_vec, BIM_SIZE_INDEX, (pshop_segment_size - BIM_SIZE_DIFF), value_bit_length);
								
		std::copy_n(pshop_vec.begin(), pshop_vec.size(), std::back_inserter(segment_vec));
	}
					
	constexpr uint16_t XMP_VEC_DEFAULT_SIZE = 405;  // XMP segment size without user data.
	const std::size_t XMP_VEC_SIZE = xmp_vec.size();

	// Are we using the second (XMP) segment?
	if (XMP_VEC_SIZE > XMP_VEC_DEFAULT_SIZE) {
	
		// Size includes segment SIG two bytes (don't count). Bluesky will strip XMP data segment greater than 60031 bytes (0xEA7F).
		// With the overhead of the XMP default segment data (405 bytes) and the Base64 encoding overhead (~33%),
		// The max compressed binary data storage in this segment is probably around ~40KB. Perhaps more?

 		constexpr uint16_t XMP_SEGMENT_SIZE_LIMIT = 60033;
 		
		if (XMP_VEC_SIZE > XMP_SEGMENT_SIZE_LIMIT) {
			throw std::runtime_error("File Size Error: Data file exceeds segment size limit for Bluesky.");
		}

		constexpr Byte 
			SIG_LENGTH 			 = 2, // FFE1
			XMP_SIZE_FIELD_INDEX = 0x02;
						
		updateValue(xmp_vec, XMP_SIZE_FIELD_INDEX, XMP_VEC_SIZE - SIG_LENGTH, value_bit_length);
			
		// Even though the order here of the split data file (if required, depending on file size) is: EXIF --> PHOTOSHOP --> XMP,
		// for compatibility requirements, the order of Segments embedded within the image file are:   EXIF --> XMP --> PHOTOSHOP.
		// To rebuild the file, if the size requires all three Segments, we take data in the order of: EXIF --> PHOTOSHOP --> XMP.
						
		constexpr auto PSHOP_SEGMENT_SIG = std::to_array<Byte>({ 0x50, 0x68, 0x6F, 0x74, 0x6F, 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E });

		constexpr Byte XMP_INSERT_INDEX_DIFF = 4;
					
		auto index_opt = searchSig(segment_vec, std::span<const Byte>(PSHOP_SEGMENT_SIG));
					
		if (!index_opt) {
    		throw std::runtime_error("Expected Photoshop segment signature not found! File is probably corrupt.");
		}
					
		const std::size_t XMP_INSERT_INDEX = *index_opt - XMP_INSERT_INDEX_DIFF;
		
		segment_vec.reserve(segment_vec.size() + xmp_vec.size());				

		segment_vec.insert(segment_vec.begin() + XMP_INSERT_INDEX, std::move_iterator(xmp_vec.begin()), std::move_iterator(xmp_vec.end()));
		std::vector<uint8_t>().swap(xmp_vec);
	}
	jpg_vec.reserve(jpg_vec.size() + segment_vec.size());	
	jpg_vec.insert(jpg_vec.begin(), std::move_iterator(segment_vec.begin()), std::move_iterator(segment_vec.end()));
	
	std::vector<uint8_t>().swap(segment_vec);
	
	platforms_vec[0] = std::move(platforms_vec[2]);
	platforms_vec.resize(1);
}

static void segmentDataFile(vBytes& segment_vec, vBytes& data_vec, vBytes& jpg_vec, vString& platforms_vec, bool hasRedditOption) {
	// Default segment_vec uses color profile segment (FFE2) to store data file. If required, split data file and use multiple segments for these larger files.
	constexpr std::size_t
		SOI_SIG_LENGTH		  = 2,
		SEGMENT_SIG_LENGTH	  = 2,
		SEGMENT_HEADER_LENGTH = 16,
		LIBSODIUM_MACBYTES 	  = 16; // 16 byte authentication tag used by libsodium. Don't count these bytes as part of the data file, they are removed during decryption.

	Byte value_bit_length = 16;
	
	std::size_t 
		segment_data_size = 65519,  // Max. copy data bytes for each segment (Not including header and signature bytes).	
		profile_with_data_vec_size = segment_vec.size(),
		max_first_segment_size = segment_data_size + SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH;
		
	// Save these two start_of_image bytes, to be restored later.
	vBytes soi_bytes(segment_vec.begin(), segment_vec.begin() + SOI_SIG_LENGTH);
	
	if (profile_with_data_vec_size > max_first_segment_size) { 
		// Data file is too large for a single icc segment, so split data file in to multiple icc segments.
		profile_with_data_vec_size -= LIBSODIUM_MACBYTES;

		std::size_t 
			// Calculate raw remainder.
			remainder_data  	   = profile_with_data_vec_size % segment_data_size,
			// Subtract header overhead, only if we have enough data to do so.
			header_overhead 	   = SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH, // 4.
			segment_remainder_size = (remainder_data > header_overhead) ? remainder_data - header_overhead : 0,
			// Calculate total segments. +1 if (usually) there is remainder data, unless perfect fit.
			total_segments		   = profile_with_data_vec_size / segment_data_size,
			segments_required	   = total_segments + (segment_remainder_size > 0);
			
		uint16_t segments_sequence_val = 1;
			
		constexpr uint16_t SEGMENTS_TOTAL_VAL_INDEX = 0x2E0;  // The value stored here is used by jdvout when extracting the data file.
		updateValue(segment_vec, SEGMENTS_TOTAL_VAL_INDEX, segments_required, value_bit_length);

		// Because of some duplicate data, erase the first 20 bytes of segment_vec because they will be replaced when splitting the data file.
    	segment_vec.erase(segment_vec.begin(), segment_vec.begin() + (SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH));

		data_vec.reserve(profile_with_data_vec_size + (segments_required * (SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH)));
		
		// ICC Profile Header...
		constexpr auto ICC_HEADER_TEMPLATE = std::to_array<Byte>({
   			0xFF, 0xE2, 0x00, 0x00,                 		// APP2 + length placeholder
    		'I','C','C','_','P','R','O','F','I','L','E',  	// 11 bytes, no null! We use that space for the 2 byte sequence number (more profile segments!).
    		0x00, 0x00,                             		// icc sequence value placeholder, 2 bytes, usng the null byte for more profiles!
    		0x01                                    		// total icc segments value. Always keep at 1, no need to update. 
		});
		
		bool isLastSegment = false;
		
		uint16_t 
			default_segment_length = static_cast<uint16_t>(segment_data_size + SEGMENT_HEADER_LENGTH), // 0xFFFF
			last_segment_length    = static_cast<uint16_t>(segment_remainder_size + SEGMENT_HEADER_LENGTH),
			segment_length		   = 0;
			
		std::size_t 
			data_size = 0,
			offset 	  = 0; 
	
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
		// Data file is small enough to fit within a single icc profile segment.
		constexpr Byte
			SEGMENT_HEADER_SIZE_INDEX = 0x04, 
			PROFILE_SIZE_INDEX  	  = 0x16;
			 
		constexpr uint16_t PROFILE_SIZE_DIFF = 16;
			
		const std::size_t
			SEGMENT_SIZE 	 	 = profile_with_data_vec_size - (SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH),
			PROFILE_SEGMENT_SIZE = SEGMENT_SIZE - PROFILE_SIZE_DIFF;

		updateValue(segment_vec, SEGMENT_HEADER_SIZE_INDEX, SEGMENT_SIZE, value_bit_length);
		updateValue(segment_vec, PROFILE_SIZE_INDEX, PROFILE_SEGMENT_SIZE, value_bit_length);
					
		data_vec.swap(segment_vec);
		vBytes().swap(segment_vec);
	}	
	constexpr uint16_t
		DEFLATED_DATA_FILE_SIZE_INDEX = 0x2E2,	// The size value stored here is used by jdvrif recover, when extracting the data file.
		PROFILE_DATA_SIZE			  = 851; 	// Color profile data size, not including user data file size.
		
	value_bit_length = 32; 
		
	updateValue(data_vec, DEFLATED_DATA_FILE_SIZE_INDEX, data_vec.size() - PROFILE_DATA_SIZE, value_bit_length);
					
	if (hasRedditOption) {	
		jpg_vec.insert(jpg_vec.begin(), soi_bytes.begin(), soi_bytes.begin() + SOI_SIG_LENGTH); 
		
		// We do the following, so that the embedded file content is preserved on Reddit after posting.
		// Without adding these padding bytes, part of the embedded file will be truncated when the image is downloaded.
		constexpr std::size_t PADDING_SIZE = 8000;
		vBytes padding_vec(PADDING_SIZE);
		
		constexpr Byte 
			PADDING_START = 33,
			PADDING_RANGE = 94,
			EOI_SIG_LENGTH = 2;
		
		for (auto& byte : padding_vec) {
    		byte = PADDING_START + static_cast<Byte>(randombytes_uniform(PADDING_RANGE));
		}
		
		jpg_vec.reserve(jpg_vec.size() + PADDING_SIZE + data_vec.size());
		jpg_vec.insert(jpg_vec.end() - EOI_SIG_LENGTH, padding_vec.begin(), padding_vec.end());
		
		vBytes().swap(padding_vec);
		
		jpg_vec.insert(jpg_vec.end() - EOI_SIG_LENGTH, std::move_iterator(data_vec.begin() + EOI_SIG_LENGTH), std::move_iterator(data_vec.end()));
		
		platforms_vec[0] = std::move(platforms_vec[5]);
		platforms_vec.resize(1);	
	} else {
		jpg_vec.reserve(jpg_vec.size() + data_vec.size());
		jpg_vec.insert(jpg_vec.begin(), std::move_iterator(data_vec.begin()), std::move_iterator(data_vec.end()));
		
		platforms_vec.erase(platforms_vec.begin() + 5); 
		platforms_vec.erase(platforms_vec.begin() + 2);
	}
	vBytes().swap(data_vec);
}	

static void binaryToBase64(vBytes& tmp_xmp_vec) {
	static constexpr char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::size_t 
		input_size  = tmp_xmp_vec.size(),
    	output_size = ((input_size + 2) / 3) * 4; 

    vBytes temp_vec(output_size); 

    std::size_t j = 0;
    						
    for (std::size_t i = 0; i < input_size; i += 3) {
    	const Byte 
			octet_a = tmp_xmp_vec[i],
        	octet_b = (i + 1 < input_size) ? tmp_xmp_vec[i + 1] : 0,
        	octet_c = (i + 2 < input_size) ? tmp_xmp_vec[i + 2] : 0;
        		
        const uint32_t triple = (static_cast<uint32_t>(octet_a) << 16) | (static_cast<uint32_t>(octet_b) << 8) | octet_c;
		
        temp_vec[j++] = base64_table[(triple >> 18) & 0x3F];
        temp_vec[j++] = base64_table[(triple >> 12) & 0x3F];
        temp_vec[j++] = (i + 1 < input_size) ? base64_table[(triple >> 6) & 0x3F] : '=';
        temp_vec[j++] = (i + 2 < input_size) ? base64_table[triple & 0x3F] : '=';
    }
	tmp_xmp_vec.swap(temp_vec);
}

static void base64ToBinary(vBytes& base64_data_vec, vBytes& pshop_tmp_vec) {
	const std::size_t input_size = base64_data_vec.size();
	
    if (input_size == 0 || (input_size % 4) != 0) {
    	throw std::invalid_argument("Base64 input size must be a multiple of 4 and non-empty");
    }

   	static constexpr int8_t base64_table[256] = {
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
	
    auto decode_val = [](unsigned char c) -> int {
        return base64_table[c];
    };

    vBytes binary_vec;
    binary_vec.reserve((input_size / 4) * 3);  

    for (std::size_t i = 0; i < input_size; i += 4) {
    	const unsigned char 
        	c0 = base64_data_vec[i],
        	c1 = base64_data_vec[i + 1],
        	c2 = base64_data_vec[i + 2],
        	c3 = base64_data_vec[i + 3];
        const bool 
        	p2 = (c2 == '='),
         	p3 = (c3 == '=');

        if (p2 && !p3) {
            throw std::invalid_argument("Invalid Base64 padding: '==' required when third char is '='");
        }
        if ((p2 || p3) && (i + 4 < input_size)) {
            throw std::invalid_argument("Padding '=' may only appear in the final quartet");
        }

        const int 
        	v0 = decode_val(c0),
        	v1 = decode_val(c1),
        	v2 = p2 ? 0 : decode_val(c2),
        	v3 = p3 ? 0 : decode_val(c3);

        if (v0 < 0 || v1 < 0 || (!p2 && v2 < 0) || (!p3 && v3 < 0)) {
            throw std::invalid_argument("Invalid Base64 character encountered");
        }

        const uint32_t triple = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12) | (static_cast<uint32_t>(v2) << 6) | static_cast<uint32_t>(v3);

        binary_vec.emplace_back(static_cast<Byte>((triple >> 16) & 0xFF));
        
        if (!p2) binary_vec.emplace_back(static_cast<Byte>((triple >> 8) & 0xFF));
        if (!p3) binary_vec.emplace_back(static_cast<Byte>(triple & 0xFF));
    }
    // Append to output
    pshop_tmp_vec.reserve(pshop_tmp_vec.size() + binary_vec.size());
    pshop_tmp_vec.insert(pshop_tmp_vec.end(), std::move_iterator(binary_vec.begin()), std::move_iterator(binary_vec.end()));
}
								
// Encrypt data file using the Libsodium cryptographic library
static std::size_t encryptDataFile(vBytes& segment_vec, vBytes& data_vec, vBytes& jpg_vec, vString& platforms_vec, std::string& data_filename, bool hasBlueskyOption, bool hasRedditOption) {
	const std::size_t
		DATA_FILENAME_XOR_KEY_INDEX = hasBlueskyOption ? 0x175 : 0x2FB,
		DATA_FILENAME_INDEX	   		= hasBlueskyOption ? 0x161 : 0x2E7,
		SODIUM_KEY_INDEX 	    	= hasBlueskyOption ? 0x18D : 0x313,
		NONCE_KEY_INDEX	            = hasBlueskyOption ? 0x1AD : 0x333;
		
	constexpr std::size_t EXIF_SEGMENT_DATA_INSERT_INDEX = 0x1D1;
				
	Byte 
		data_filename_length = segment_vec[DATA_FILENAME_INDEX - 1],
		value_bit_length 	 = 32;
		
	randombytes_buf(segment_vec.data() + DATA_FILENAME_XOR_KEY_INDEX, data_filename_length);

	std::transform(data_filename.begin(), data_filename.begin() + data_filename_length, 
		segment_vec.begin() + DATA_FILENAME_XOR_KEY_INDEX, segment_vec.begin() + DATA_FILENAME_INDEX, 
			[](char a, Byte b) { return static_cast<Byte>(a) ^ b; }
    );	
	
	Key   key{};
	Nonce nonce{};
			
    crypto_secretbox_keygen(key.data());
   	randombytes_buf(nonce.data(), nonce.size());

	std::copy_n(key.begin(), crypto_secretbox_KEYBYTES, segment_vec.begin() + SODIUM_KEY_INDEX); 	
	std::copy_n(nonce.begin(), crypto_secretbox_NONCEBYTES, segment_vec.begin() + NONCE_KEY_INDEX);
	
	const std::size_t data_length = data_vec.size();
	
	data_vec.resize(data_length + TAG_BYTES);

    if (crypto_secretbox_easy(data_vec.data(), data_vec.data(), data_length, nonce.data(), key.data()) != 0) {
    	sodium_memzero(key.data(),   key.size());
        sodium_memzero(nonce.data(), nonce.size());
        throw std::runtime_error("crypto_secretbox_easy failed");
    }				
    	
    segment_vec.reserve(segment_vec.size() + data_vec.size());
     	
	vBytes bluesky_xmp_vec;
	vBytes bluesky_pshop_vec;
			
	if (hasBlueskyOption) { 
		// User has selected the -b argument option for the Bluesky platform.
		// + With EXIF overhead segment data (511) - four bytes we don't count (FFD8 FFE1),  
		// = Max. segment size 65534 (0xFFFE). Can't have 65535 (0xFFFF) as Bluesky will strip the EXIF segment.
		constexpr uint16_t COMPRESSED_FILE_SIZE_INDEX = 0x1CD;

		constexpr std::size_t EXIF_SEGMENT_DATA_SIZE_LIMIT = 65027;
		
		const std::size_t ENCRYPTED_VEC_SIZE = data_vec.size();
						 	 
		updateValue(segment_vec, COMPRESSED_FILE_SIZE_INDEX, ENCRYPTED_VEC_SIZE, value_bit_length);

		// Split the data file if it exceeds the Max. compressed EXIF capacity of ~64KB. 
		// We can use the Photoshop segment to store more data, again ~64KB Max. stored as two ~32KB datasets within the segment.
		// If the data file exceeds the Photoshop segement, we can then try and fit the remaining data in the XMP segment (Base64 encoded).
		// EXIF (~64KB) --> Photoshop (~64KB (2x ~32KB datasets)) --> XMP (~42KB (data encoded and stored as Base64)). Max. ~170KB.

		if (ENCRYPTED_VEC_SIZE > EXIF_SEGMENT_DATA_SIZE_LIMIT) {	
			segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, data_vec.begin(), data_vec.begin() + EXIF_SEGMENT_DATA_SIZE_LIMIT);

			std::size_t 
				remaining_data_size = ENCRYPTED_VEC_SIZE - EXIF_SEGMENT_DATA_SIZE_LIMIT,
				data_file_index = EXIF_SEGMENT_DATA_SIZE_LIMIT;
						
			constexpr std::size_t
				FIRST_DATASET_SIZE_LIMIT = 32767, // 0x7FFF
				LAST_DATASET_SIZE_LIMIT  = 32730; // 0x7FDA 
						
			constexpr Byte FIRST_DATASET_SIZE_INDEX = 0x21;
						
			value_bit_length = 16;
						
			bluesky_pshop_vec = { 
				0xFF, 0xED, 0x00, 0x21, 0x50, 0x68, 0x6F, 0x74, 0x6F, 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E, 0x30, 0x00, 0x38, 0x42, 0x49,
				0x4D, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x1C, 0x08, 0x0A, 0x00, 0x00
			};
						
			const std::size_t first_copy_size = std::min(FIRST_DATASET_SIZE_LIMIT, remaining_data_size);
			
        	updateValue(bluesky_pshop_vec, FIRST_DATASET_SIZE_INDEX, first_copy_size, value_bit_length);
        	bluesky_pshop_vec.insert(bluesky_pshop_vec.end(), data_vec.begin() + data_file_index, data_vec.begin() + data_file_index + first_copy_size);
						
			if (remaining_data_size > FIRST_DATASET_SIZE_LIMIT) {	
				remaining_data_size -= FIRST_DATASET_SIZE_LIMIT;
				data_file_index += FIRST_DATASET_SIZE_LIMIT;
							
				// Add an additional (final) dataset to the bluesky_pshop_vec
				constexpr Byte DATASET_SIZE_INDEX = 3;
								
				vBytes dataset_marker_vec { 0x1C, 0x08, 0x0A, 0x00, 0x00 }; // 3 byte dataset ID, 2 byte length field.
							
				const std::size_t last_copy_size = std::min(LAST_DATASET_SIZE_LIMIT, remaining_data_size);
            	updateValue(dataset_marker_vec, DATASET_SIZE_INDEX, last_copy_size, value_bit_length);
            	
            	bluesky_pshop_vec.insert(bluesky_pshop_vec.end(), dataset_marker_vec.begin(), dataset_marker_vec.end());
            	bluesky_pshop_vec.insert(bluesky_pshop_vec.end(), data_vec.begin() + data_file_index, data_vec.begin() + data_file_index + last_copy_size);
							
				if (remaining_data_size > LAST_DATASET_SIZE_LIMIT) {	
					remaining_data_size -= LAST_DATASET_SIZE_LIMIT;
					data_file_index += LAST_DATASET_SIZE_LIMIT;
								
					vBytes tmp_xmp_vec(remaining_data_size);
			
					std::copy_n(data_vec.begin() + data_file_index, remaining_data_size, tmp_xmp_vec.begin());
			
					// We can only store Base64 encoded data in the XMP segment, so convert the binary data here.
					binaryToBase64(tmp_xmp_vec);
    					
					constexpr std::size_t XMP_SEGMENT_DATA_INSERT_INDEX = 0x139;
							
					// XMP (FFE1) segment.
					// Notes: 	Data file index = 0x139 (Remainder part of data file stored here if too big for Photoshop segment (bluesky_pshop_vec).
					//	  		Data file content stored here as BASE64).
							
					bluesky_xmp_vec = { 
						0xFF, 0xE1, 0x01, 0x93, 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F, 0x2F, 0x6E, 0x73, 0x2E, 0x61, 0x64, 0x6F, 0x62, 0x65, 0x2E, 0x63,
						0x6F, 0x6D, 0x2F, 0x78, 0x61, 0x70, 0x2F, 0x31, 0x2E, 0x30, 0x2F, 0x00, 0x3C, 0x3F, 0x78, 0x70, 0x61, 0x63, 0x6B, 0x65, 0x74,
						0x20, 0x62, 0x65, 0x67, 0x69, 0x6E, 0x3D, 0x22, 0x22, 0x20, 0x69, 0x64, 0x3D, 0x22, 0x57, 0x35, 0x4D, 0x30, 0x4D, 0x70, 0x43,
						0x65, 0x68, 0x69, 0x48, 0x7A, 0x72, 0x65, 0x53, 0x7A, 0x4E, 0x54, 0x63, 0x7A, 0x6B, 0x63, 0x39, 0x64, 0x22, 0x3F, 0x3E, 0x0A,
						0x3C, 0x78, 0x3A, 0x78, 0x6D, 0x70, 0x6D, 0x65, 0x74, 0x61, 0x20, 0x78, 0x6D, 0x6C, 0x6E, 0x73, 0x3A, 0x78, 0x3D, 0x22, 0x61,
						0x64, 0x6F, 0x62, 0x65, 0x3A, 0x6E, 0x73, 0x3A, 0x6D, 0x65, 0x74, 0x61, 0x2F, 0x22, 0x20, 0x78, 0x3A, 0x78, 0x6D, 0x70, 0x74,
						0x6B, 0x3D, 0x22, 0x47, 0x6F, 0x20, 0x58, 0x4D, 0x50, 0x20, 0x53, 0x44, 0x4B, 0x20, 0x31, 0x2E, 0x30, 0x22, 0x3E, 0x3C, 0x72,
						0x64, 0x66, 0x3A, 0x52, 0x44, 0x46, 0x20, 0x78, 0x6D, 0x6C, 0x6E, 0x73, 0x3A, 0x72, 0x64, 0x66, 0x3D, 0x22, 0x68, 0x74, 0x74,
						0x70, 0x3A, 0x2F, 0x2F, 0x77, 0x77, 0x77, 0x2E, 0x77, 0x33, 0x2E, 0x6F, 0x72, 0x67, 0x2F, 0x31, 0x39, 0x39, 0x39, 0x2F, 0x30,
						0x32, 0x2F, 0x32, 0x32, 0x2D, 0x72, 0x64, 0x66, 0x2D, 0x73, 0x79, 0x6E, 0x74, 0x61, 0x78, 0x2D, 0x6E, 0x73, 0x23, 0x22, 0x3E,
						0x3C, 0x72, 0x64, 0x66, 0x3A, 0x44, 0x65, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x69, 0x6F, 0x6E, 0x20, 0x78, 0x6D, 0x6C, 0x6E,
						0x73, 0x3A, 0x64, 0x63, 0x3D, 0x22, 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F, 0x2F, 0x70, 0x75, 0x72, 0x6C, 0x2E, 0x6F, 0x72, 0x67,
						0x2F, 0x64, 0x63, 0x2F, 0x65, 0x6C, 0x65, 0x6D, 0x65, 0x6E, 0x74, 0x73, 0x2F, 0x31, 0x2E, 0x31, 0x2F, 0x22, 0x20, 0x72, 0x64,
						0x66, 0x3A, 0x61, 0x62, 0x6F, 0x75, 0x74, 0x3D, 0x22, 0x22, 0x3E, 0x3C, 0x64, 0x63, 0x3A, 0x63, 0x72, 0x65, 0x61, 0x74, 0x6F,
						0x72, 0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x53, 0x65, 0x71, 0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69, 0x3E, 0x3C, 0x2F,
						0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69, 0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x53, 0x65, 0x71, 0x3E, 0x3C, 0x2F, 0x64, 0x63,
						0x3A, 0x63, 0x72, 0x65, 0x61, 0x74, 0x6F, 0x72, 0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x44, 0x65, 0x73, 0x63, 0x72, 0x69,
						0x70, 0x74, 0x69, 0x6F, 0x6E, 0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x52, 0x44, 0x46, 0x3E, 0x3C, 0x2F, 0x78, 0x3A, 0x78,
						0x6D, 0x70, 0x6D, 0x65, 0x74, 0x61, 0x3E, 0x0A, 0x3C, 0x3F, 0x78, 0x70, 0x61, 0x63, 0x6B, 0x65, 0x74, 0x20, 0x65, 0x6E, 0x64,
						0x3D, 0x22, 0x77, 0x22, 0x3F, 0x3E
					};
					// Store the second part of the file (as Base64) within the XMP segment.
					bluesky_xmp_vec.insert(bluesky_xmp_vec.begin() + XMP_SEGMENT_DATA_INSERT_INDEX, std::move_iterator(tmp_xmp_vec.begin()), std::move_iterator(tmp_xmp_vec.end()));
				}
			}		
		} else { 
			// Data file was small enough to fit within the EXIF segment, XMP segment not required.
			segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, std::move_iterator(data_vec.begin()), std::move_iterator(data_vec.end()));
		}	
	} else { 
		// No option selected so used the default color profile segment for data storage instead of exif (bluesky -b).
		segment_vec.insert(segment_vec.end(), std::move_iterator(data_vec.begin()), std::move_iterator(data_vec.end()));
	}	
	vBytes().swap(data_vec);
	constexpr Byte SODIUM_XOR_KEY_LENGTH = 8;
	
	std::size_t 
		pin 			   = getValue(segment_vec, SODIUM_KEY_INDEX, SODIUM_XOR_KEY_LENGTH),
		sodium_xor_key_pos = SODIUM_KEY_INDEX,
		sodium_key_pos 	   = SODIUM_KEY_INDEX;

	Byte sodium_keys_length = 48;
	
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
   		updateBlueskySegmentValues(segment_vec, bluesky_pshop_vec, bluesky_xmp_vec, jpg_vec, platforms_vec);
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
	const std::size_t 
		SODIUM_KEY_INDEX = isBlueskyFile ? 0x18D : 0x2FB,
		NONCE_KEY_INDEX  = isBlueskyFile ? 0x1AD : 0x31B;

	Byte 
		sodium_keys_length = 48,
		value_bit_length   = 64;
			
	std::size_t recovery_pin = getPin();
			
	updateValue(jpg_vec, SODIUM_KEY_INDEX, recovery_pin, value_bit_length);
			
	constexpr Byte SODIUM_XOR_KEY_LENGTH = 8; 
			
	std::size_t 
		sodium_xor_key_pos = SODIUM_KEY_INDEX,
		sodium_key_pos 	   = SODIUM_KEY_INDEX + SODIUM_XOR_KEY_LENGTH;
			
	while(sodium_keys_length--) {
		jpg_vec[sodium_key_pos] ^= jpg_vec[sodium_xor_key_pos++];
		sodium_key_pos++;
		if (sodium_xor_key_pos >= SODIUM_KEY_INDEX + SODIUM_XOR_KEY_LENGTH) {
			sodium_xor_key_pos = SODIUM_KEY_INDEX;
		}
	}

	Key key{};
	Nonce nonce{};
			
	std::copy_n(jpg_vec.begin() + SODIUM_KEY_INDEX, key.size(), key.data());
	std::copy_n(jpg_vec.begin() + NONCE_KEY_INDEX, nonce.size(), nonce.data());

	const std::size_t
		ENCRYPTED_FILENAME_INDEX = isBlueskyFile ? 0x161 : 0x2CF,
		FILENAME_XOR_KEY_INDEX   = isBlueskyFile ? 0x175 : 0x2E3,
		FILE_SIZE_INDEX 	 	 = isBlueskyFile ? 0x1CD : 0x2CA,
		FILENAME_LENGTH_INDEX    = ENCRYPTED_FILENAME_INDEX - 1;
		
    const Byte FILENAME_LENGTH = jpg_vec[FILENAME_LENGTH_INDEX];

	std::string decrypted_filename;
 	decrypted_filename.resize(FILENAME_LENGTH);

    for (std::size_t i = 0; i < FILENAME_LENGTH; ++i) {
    	decrypted_filename[i] = static_cast<char>(jpg_vec[ENCRYPTED_FILENAME_INDEX + i] ^ jpg_vec[FILENAME_XOR_KEY_INDEX + i]);
    }
			
	constexpr uint16_t TOTAL_PROFILE_HEADER_SEGMENTS_INDEX = 0x2C8;
	Byte byte_length = 2;
	
	const std::size_t ENCRYPTED_FILE_START_INDEX = isBlueskyFile ? 0x1D1 : 0x33B;
	
	const uint16_t TOTAL_PROFILE_HEADER_SEGMENTS = static_cast<uint16_t>(getValue(jpg_vec, TOTAL_PROFILE_HEADER_SEGMENTS_INDEX, byte_length));		
	
	constexpr std::size_t COMMON_DIFF_VAL = 65537; // ICC segment spacing. Size difference between each icc segment profile header.
	byte_length = 4;
	
	const std::size_t 
		EMBEDDED_FILE_SIZE = getValue(jpg_vec, FILE_SIZE_INDEX, byte_length),
		LAST_SEGMENT_INDEX = (static_cast<std::size_t>(TOTAL_PROFILE_HEADER_SEGMENTS) - 1) * COMMON_DIFF_VAL - 0x16;
	
	// Check embedded data file for corruption, such as missing data segments.
	// Why? If you post an embedded image to Mastodon, which exceeds the icc segment limit of 100 (~6MB), 
	// it will allow the post if it does not exceed the image size limit (16MB), but it will truncate the segments over 100.
	// When the image is downloaded and an attempt is made to recover the embedded file, it will fail because of the missing segments.
	// Also, general corruption could cause this.
	
	if (TOTAL_PROFILE_HEADER_SEGMENTS && !isBlueskyFile) {
		if (LAST_SEGMENT_INDEX >= jpg_vec.size() || jpg_vec[LAST_SEGMENT_INDEX] != 0xFF || jpg_vec[LAST_SEGMENT_INDEX + 1] != 0xE2) {
			throw std::runtime_error("File Extraction Error: Missing segments detected. Embedded data file is corrupt!");
		}
	}
	
    // Copy the wanted bytes to the front, then resize.
	std::copy(jpg_vec.begin() + ENCRYPTED_FILE_START_INDEX, jpg_vec.begin() + ENCRYPTED_FILE_START_INDEX + EMBEDDED_FILE_SIZE, jpg_vec.begin());
	
	jpg_vec.resize(EMBEDDED_FILE_SIZE);
	jpg_vec.shrink_to_fit();
	
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
		std::size_t header_index = 0xFCB0; // The first split segment profile header location, this is after the main header/icc profile, which was previously removed.
	
		constexpr std::size_t PROFILE_HEADER_LENGTH = 18;
		
		const std::size_t LIMIT = jpg_vec.size();
		
		std::size_t  
			read_pos    = 0,                 
			write_pos   = 0,                 
			next_header = header_index;		
			
		// We need to avoid including the icc segment profile headers within the decrypted output file.
		// Because we know the total number of profile headers and their location (common difference val), 
		// we can just skip the header bytes when copying the data to the sanitize vector.
        // This is much faster than having to search for and then using something like vec.erase to remove the header string from the vector.

		while (read_pos < LIMIT) {
    		if (read_pos == next_header) {
        		// Skip the header bytes.
        		read_pos += std::min(PROFILE_HEADER_LENGTH, LIMIT - read_pos);
        		next_header += COMMON_DIFF_VAL;
        		continue;
    		}
    		jpg_vec[write_pos++] = jpg_vec[read_pos++];  
		}
		if (hasNoProfileHeaders) {
			jpg_vec.resize(jpg_vec.size());
		} else {
			jpg_vec.resize(write_pos);
		}               
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
	constexpr std::size_t BUFSIZE = 2 * 1024 * 1024; // 2 MB.  
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
        if (deflateInit(&strm, Z_BEST_COMPRESSION) != Z_OK) {
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
    data_vec.swap(output_vec);
}

static vBytes readFile(const fs::path& path, Byte file_type_and_mode = 3) {	
	if (!fs::exists(path.string()) || !fs::is_regular_file(path.string())) {
        throw std::runtime_error("Error: File \"" + path.string() + "\" not found or not a regular file.");
    }

	std::size_t file_size = fs::file_size(path.string());

	if (!file_size) {
		throw std::runtime_error("Error: File is empty.");
    }
    	
    if (file_type_and_mode != 3) {
    	// Some platform apps, such as Bluesky mobile and X-Twitter mobile, will often save .jpg images with a .png extension, even though the content/file type is still JPEG.
    	// That is why I allow for the .png extension here, although the content must still be JPEG, as the file will be checked for the correct format later.
    	if (!hasFileExtension(path.string(), {".png", ".jpg", ".jpeg", ".jfif"})) {
        	throw std::runtime_error("File Type Error: Invalid image extension. Only expecting \".jpg\", \".jpeg\", \".jfif\" or \".png\".");
    	}
    		
    	if (file_type_and_mode == 1) {
			constexpr std::size_t MINIMUM_IMAGE_SIZE = 134;
			
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
		hasNoOption 	 = (option == Option::None),
		hasBlueskyOption = (option == Option::Bluesky),
		hasRedditOption  = (option == Option::Reddit);
			
	int 
		width  = 0, 
		height = 0;	
	
	optimizeImage(jpg_vec, width, height, hasNoOption);
			
	constexpr auto 
		DQT1_SIG = std::to_array<Byte>({ 0xFF, 0xDB, 0x00, 0x43 }),	// Define Quantization Tables SIG.
		DQT2_SIG = std::to_array<Byte>({ 0xFF, 0xDB, 0x00, 0x84 });
				
    auto 
    	dqt1 = searchSig(jpg_vec, std::span<const Byte>(DQT1_SIG)),
    	dqt2 = searchSig(jpg_vec, std::span<const Byte>(DQT2_SIG));

	if (!dqt1 && !dqt2) {
    	throw std::runtime_error("Image File Error: No DQT segment found (corrupt or unsupported JPG).");
	}

	const std::size_t NPOS = static_cast<std::size_t>(-1);
			
	std::size_t dqt_pos = std::min(dqt1.value_or(NPOS), dqt2.value_or(NPOS));
			
	jpg_vec.erase(jpg_vec.begin(), jpg_vec.begin() + static_cast<std::ptrdiff_t>(dqt_pos));

	std::size_t jpg_size = jpg_vec.size(); // Update image size again after image optimization, etc. 
	
	constexpr std::size_t
		MAX_OPTIMIZED_IMAGE_SIZE	= 4ULL * 1024 * 1024,	// 4 MB.
		MAX_OPTIMIZED_BLUESKY_IMAGE	= 805  * 1024;			// 805 KB.
		
	if (jpg_size > MAX_OPTIMIZED_IMAGE_SIZE) {
		throw std::runtime_error("Image File Error: Cover image file exceeds maximum size limit.");
	}
			
	if (hasBlueskyOption && jpg_size > MAX_OPTIMIZED_BLUESKY_IMAGE) {
		throw std::runtime_error("File Size Error: Image file exceeds maximum size limit for the Bluesky platform.");
	}
			
    vBytes data_vec = readFile(data_file_path);
	std::size_t data_size = data_vec.size();
	
	constexpr Byte DATA_FILENAME_MAX_LENGTH = 20;

	std::string data_filename = data_file_path.filename().string();

	if (data_filename.size() > DATA_FILENAME_MAX_LENGTH) {
    	throw std::runtime_error("Data File Error: For compatibility requirements, length of data filename must not exceed 20 characters.");
	}
						
    isCompressedFile = hasFileExtension(data_file_path, {".zip",".jar",".rar",".7z",".bz2",".gz",".xz",".tar",".lz",".lz4",".cab",".rpm",".deb", ".mp4",".mp3",".exe",".jpg",".jpeg",".jfif",".png",".webp",".bmp",".gif",".ogg",".flac"});
    											
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
		0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x00, 0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x4D, 0x4D, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x06, 0x01, 0x12,
		0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x1A, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x1B,
		0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x28, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x01, 0x3B,
		0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x87, 0x69, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
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
			
	Byte value_bit_length = 16;
				
	if (hasBlueskyOption) {
		// Use the EXIF segment instead of the default color profile segment to store user data.
		// The color profile segment (FFE2) is removed by Bluesky, so we use EXIF.
		segment_vec.swap(bluesky_exif_vec);	
				
		constexpr uint16_t
			BLUESKY_VEC_HEIGHT_INDEX = 0x1F9,
			BLUESKY_VEC_WIDTH_INDEX  = 0x1ED;
				
		updateValue(segment_vec, BLUESKY_VEC_HEIGHT_INDEX, height, value_bit_length);
		updateValue(segment_vec, BLUESKY_VEC_WIDTH_INDEX, width, value_bit_length);
	} 
	
	vBytes().swap(bluesky_exif_vec);
			
	const std::size_t DATA_FILENAME_LENGTH_INDEX = hasBlueskyOption ? 0x160 : 0x2E6;

	segment_vec[DATA_FILENAME_LENGTH_INDEX] = static_cast<Byte>(data_filename.size());	 
		
	constexpr std::size_t LARGE_FILE_SIZE = 300ULL * 1024 * 1024; // 300 MB
       		
	if (data_size > LARGE_FILE_SIZE) {
		std::cout << "\nPlease wait. Larger files will take longer to complete this process.\n";	
	}
			
	if (isCompressedFile) {
		// Skip Zlib deflate. Data file probably already compressed.
		std::size_t no_compression_marker_index = (hasBlueskyOption) ? 0x14B : 0x80;
		segment_vec[no_compression_marker_index] = 0x58; // ID byte for recovery mode. Will skip Zlib inflate.
	} else {
		// Deflate data file with Zlib.
		zlibFunc(data_vec, mode);
		data_size = data_vec.size(); // Get new size after deflate.
	}
		
	constexpr std::size_t 
		MAX_SIZE_CONCEAL 	  = 2ULL 	* 1024 * 1024 * 1024,  // 2GB.
		MAX_SIZE_REDDIT 	  = 20ULL   * 1024 * 1024,   	   // 20MB.
		MAX_DATA_SIZE_BLUESKY = 2ULL    * 1024 * 1024;		   // 2MB. 
				
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
	
	const std::size_t EMBEDDED_JPG_SIZE = jpg_vec.size();
	
	const uint32_t RAND_NUM = 100000 + randombytes_uniform(900000);
	const std::string OUTPUT_FILENAME = "jrif_" + std::to_string(RAND_NUM) + ".jpg";
	
	std::ofstream file_ofs(OUTPUT_FILENAME, std::ios::binary);

	if (!file_ofs) {
		throw std::runtime_error("Write File Error: Unable to write to file. Make sure you have WRITE permissions for this location.");
	}
	
	file_ofs.write(reinterpret_cast<const char*>(jpg_vec.data()), EMBEDDED_JPG_SIZE);
	file_ofs.close();
	
	if (hasNoOption) {
		constexpr std::size_t 
			FLICKR_MAX_IMAGE_SIZE 		   = 200ULL * 1024 * 1024,
			IMGPILE_MAX_IMAGE_SIZE 		   = 100ULL * 1024 * 1024,
			IMGBB_POSTIMAGE_MAX_IMAGE_SIZE = 32ULL  * 1024 * 1024,
			MASTODON_MAX_IMAGE_SIZE 	   = 16ULL  * 1024 * 1024,
			PIXELFED_MAX_IMAGE_SIZE 	   = 15ULL  * 1024 * 1024,
			TWITTER_MAX_IMAGE_SIZE 		   = 5ULL   * 1024 * 1024,
			TWITTER_MAX_DATA_SIZE 		   = 10  	* 1024,	
			TUMBLR_MAX_DATA_SIZE 		   = 65534,	
			MASTODON_MAX_SEGMENTS 	 	   = 100;
			
		constexpr uint16_t TOTAL_SEGMENTS_INDEX = 0x2E0;
			
		constexpr Byte 
			FIRST_SEGMENT_SIZE_INDEX = 0x04,
			VALUE_LENGTH			 = 2;
		
		const uint16_t
			FIRST_SEGMENT_SIZE = getValue(jpg_vec, FIRST_SEGMENT_SIZE_INDEX, VALUE_LENGTH),
			TOTAL_SEGMENTS 	   = getValue(jpg_vec, TOTAL_SEGMENTS_INDEX, VALUE_LENGTH);
		
		std::vector<std::string> filtered_platforms;

		for (const std::string& platform : platforms_vec) {
    		if (platform == "X-Twitter" && (FIRST_SEGMENT_SIZE > TWITTER_MAX_DATA_SIZE || EMBEDDED_JPG_SIZE > TWITTER_MAX_IMAGE_SIZE)) {
        		continue;
    		}
    		if (platform == "Tumblr" && (FIRST_SEGMENT_SIZE > TUMBLR_MAX_DATA_SIZE)) {
        		continue;
    		}
    		if (platform == "Mastodon" && (TOTAL_SEGMENTS > MASTODON_MAX_SEGMENTS || EMBEDDED_JPG_SIZE > MASTODON_MAX_IMAGE_SIZE)) {
        		continue;
    		}
			if (platform == "Pixelfed" && EMBEDDED_JPG_SIZE > PIXELFED_MAX_IMAGE_SIZE) {
				continue;
			}
    		if ((platform == "ImgBB" || platform == "PostImage") && (EMBEDDED_JPG_SIZE > IMGBB_POSTIMAGE_MAX_IMAGE_SIZE)) {
        		continue;
    		}
    		if (platform == "ImgPile" && EMBEDDED_JPG_SIZE > IMGPILE_MAX_IMAGE_SIZE) {
        		continue;
    		}
    		if (platform == "Flickr" && EMBEDDED_JPG_SIZE > FLICKR_MAX_IMAGE_SIZE) {
        		continue;
    		}
			filtered_platforms.emplace_back(platform);
		}
		if (filtered_platforms.empty()) {
    		filtered_platforms.emplace_back("\b\bUnknown!\n\n Due to the large file size of the output JPG image, I'm unaware of any\n compatible platforms that this image can be posted on. Local use only?");
		}
		platforms_vec.swap(filtered_platforms);
	}
	std::cout << "\nPlatform compatibility for output image:-\n\n";
			
	for (const auto& s : platforms_vec) {
        std::cout << " ✓ "<< s << '\n' ;
   	}	
   		 	
	std::cout << "\nSaved \"file-embedded\" JPG image: " << OUTPUT_FILENAME  << " (" << EMBEDDED_JPG_SIZE << " bytes).\n";
	std::cout << "\nRecovery PIN: [***" << recovery_pin << "***]\n\nImportant: Keep your PIN safe, so that you can extract the hidden file.\n\nComplete!\n\n";
	return 0;
}

static int recoverData(vBytes& jpg_vec, Mode mode, fs::path& image_file_path) {
	constexpr std::size_t 
		SIG_LENGTH = 7,
		INDEX_DIFF = 8;

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
		const std::size_t ICC_PROFILE_SIG_INDEX = *index_opt;			
		jpg_vec.erase(jpg_vec.begin(), jpg_vec.begin() + (ICC_PROFILE_SIG_INDEX - INDEX_DIFF));		
		isBlueskyFile = false;
	}
		
	const std::size_t COMPRESSION_MARKER_INDEX = isBlueskyFile ? 0x14B : 0x68;
	if (jpg_vec[COMPRESSION_MARKER_INDEX] == 0x58) isDataCompressed = false;

	if (isBlueskyFile) { // EXIF segment (FFE1) is being used instead of ICC (FFE2). Also check for PHOTOSHOP & XMP segments and their index locations.
    	constexpr auto 
			PSHOP_SIG       = std::to_array<Byte>({ 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E }),
			XMP_CREATOR_SIG = std::to_array<Byte>({ 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69 });

    	index_opt = searchSig(jpg_vec, std::span<const Byte>(PSHOP_SIG));
    		
    	if (index_opt) {
    		// Found Photoshop segment.
        	constexpr std::size_t 
        		MAX_SINGLE_DATASET_PSHOP_SEGMENT_SIZE = 32800, // If the photoshop segment size is greater than this size, we have two datasets (max).
            	PSHOP_SEGMENT_SIZE_INDEX_DIFF 		  = 7,
            	PSHOP_FIRST_DATASET_SIZE_INDEX_DIFF   = 24,
            	PSHOP_DATASET_FILE_INDEX_DIFF 		  = 2;
        
        	constexpr Byte BYTE_LENGTH = 2;

        	const std::size_t
            	PSHOP_SIG_INDEX 			   = *index_opt,
            	PSHOP_SEGMENT_SIZE_INDEX 	   = PSHOP_SIG_INDEX - PSHOP_SEGMENT_SIZE_INDEX_DIFF,
            	PSHOP_FIRST_DATASET_SIZE_INDEX = PSHOP_SIG_INDEX + PSHOP_FIRST_DATASET_SIZE_INDEX_DIFF,
            	PSHOP_FIRST_DATASET_FILE_INDEX = PSHOP_FIRST_DATASET_SIZE_INDEX + PSHOP_DATASET_FILE_INDEX_DIFF;

        	const uint16_t
            	PSHOP_SEGMENT_SIZE 	 	 = static_cast<uint16_t>(getValue(jpg_vec, PSHOP_SEGMENT_SIZE_INDEX, BYTE_LENGTH)),
            	PSHOP_FIRST_DATASET_SIZE = static_cast<uint16_t>(getValue(jpg_vec, PSHOP_FIRST_DATASET_SIZE_INDEX, BYTE_LENGTH));

        	constexpr std::size_t END_EXIF_DATA_INDEX_DIFF = 55;
        		
        	const std::size_t END_EXIF_DATA_INDEX = PSHOP_SIG_INDEX - END_EXIF_DATA_INDEX_DIFF;

        	if (MAX_SINGLE_DATASET_PSHOP_SEGMENT_SIZE >= PSHOP_SEGMENT_SIZE) {
        		// Just a single dataset. Copy it and finish here...
        		std::copy_n(jpg_vec.begin() + PSHOP_FIRST_DATASET_FILE_INDEX, PSHOP_FIRST_DATASET_SIZE, jpg_vec.begin() + END_EXIF_DATA_INDEX);
        	} else {
				// We have a second dataset for the photoshop segment...
            	vBytes pshop_tmp_vec;
            	pshop_tmp_vec.reserve(PSHOP_FIRST_DATASET_SIZE);
            	
            	pshop_tmp_vec.insert(pshop_tmp_vec.end(), jpg_vec.begin() + PSHOP_FIRST_DATASET_FILE_INDEX, jpg_vec.begin() + PSHOP_FIRST_DATASET_FILE_INDEX + PSHOP_FIRST_DATASET_SIZE);

            	constexpr std::size_t PSHOP_LAST_DATASET_SIZE_INDEX_DIFF = 3;
            			
            	const std::size_t
                	PSHOP_LAST_DATASET_SIZE_INDEX = PSHOP_FIRST_DATASET_FILE_INDEX + PSHOP_FIRST_DATASET_SIZE + PSHOP_LAST_DATASET_SIZE_INDEX_DIFF,
                	PSHOP_LAST_DATASET_FILE_INDEX = PSHOP_LAST_DATASET_SIZE_INDEX + PSHOP_DATASET_FILE_INDEX_DIFF;

            	const uint16_t PSHOP_LAST_DATASET_SIZE = static_cast<uint16_t>(getValue(jpg_vec, PSHOP_LAST_DATASET_SIZE_INDEX, BYTE_LENGTH));

            	pshop_tmp_vec.reserve(pshop_tmp_vec.size() + PSHOP_LAST_DATASET_SIZE);
            	
            	pshop_tmp_vec.insert(pshop_tmp_vec.end(), jpg_vec.begin() + PSHOP_LAST_DATASET_FILE_INDEX, jpg_vec.begin() + PSHOP_LAST_DATASET_FILE_INDEX + PSHOP_LAST_DATASET_SIZE);

				// Now check to see if we have an XMP segment. (Always base64 data).
            	index_opt = searchSig(jpg_vec, std::span<const Byte>(XMP_CREATOR_SIG));
            			
            	if (!index_opt) {
                	std::copy_n(pshop_tmp_vec.begin(), pshop_tmp_vec.size(), jpg_vec.begin() + END_EXIF_DATA_INDEX);
            	} else {
					// Found XMP segment.
                	const std::size_t
                    	XMP_CREATOR_SIG_INDEX 	= *index_opt,
                    	BEGIN_BASE64_DATA_INDEX = XMP_CREATOR_SIG_INDEX + SIG_LENGTH + 1;

                	constexpr Byte END_BASE64_DATA_SIG = 0x3C;
                			
                	const std::size_t 
                		END_BASE64_DATA_SIG_INDEX = static_cast<std::size_t>(std::find(jpg_vec.begin() + BEGIN_BASE64_DATA_INDEX, jpg_vec.end(), END_BASE64_DATA_SIG) - jpg_vec.begin()),
                		BASE64_DATA_SIZE 		  = END_BASE64_DATA_SIG_INDEX - BEGIN_BASE64_DATA_INDEX;
                			
  			vBytes base64_data_vec(BASE64_DATA_SIZE);
                	std::copy_n(jpg_vec.begin() + BEGIN_BASE64_DATA_INDEX, BASE64_DATA_SIZE, base64_data_vec.begin());
                			
					// Convert the XMP base64 data segment back to binary.
                	base64ToBinary(base64_data_vec, pshop_tmp_vec);

                	constexpr std::size_t END_EXIF_DATA_INDEX_DIFF = 351;
                			
                	const std::size_t END_EXIF_DATA_INDEX = XMP_CREATOR_SIG_INDEX - END_EXIF_DATA_INDEX_DIFF;
                			
					// Append the binary data from multiple segments (pshop (2x datasets) + xmp) to the EXIF binary segment data. We now have the complete data file.
                	// std::copy_n(pshop_tmp_vec.begin(), pshop_tmp_vec.size(), jpg_vec.begin() + END_EXIF_DATA_INDEX);
                	jpg_vec.insert(jpg_vec.begin() + END_EXIF_DATA_INDEX, std::move_iterator(pshop_tmp_vec.begin()), std::move_iterator(pshop_tmp_vec.end()));
            	}
        	}
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
		
    	Byte file_type_and_mode = isConcealMode ? 1 : 2;
    			
		vBytes jpg_vec = readFile(args.image_file_path, file_type_and_mode);
			
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

