// JPG Data Vehicle (jdvrif v5.4) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

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
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream> 
#include <iostream>
#include <iterator> 
#include <random> 
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>    

namespace fs = std::filesystem;

static inline void displayInfo() {
	std::cout << R"(

JPG Data Vehicle (jdvrif v5.4)
Created by Nicholas Cleasby (@CleasbyCode) 24/01/2023

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

  • Flickr 		(200 MB)
  • ImgPile 	(100 MB)
  • ImgBB 		(32 MB)
  • PostImage 	(32 MB)
  • Reddit 		(20 MB) — (use -r option).

Limit measured by compressed data file size only:

  • Mastodon 	(~6 MB)
  • Tumblr 		(~64 KB)
  • X-Twitter 	(~10 KB)

For example, on Mastodon, even if your cover image is 1 MB, you can still embed a data file
up to the ~6 MB Mastodon size limit.

Other: 

Bluesky - Separate size limits for cover image and data file - (use -b option).
  • Cover image: 800 KB
  • Secret data file (compressed): ~171 KB

Even though jdvrif compresses the data file, you may want to compress it yourself first
(zip, rar, 7z, etc.) so you know the exact compressed file size.

Platforms with small size limits, like X-Twitter (~10 KB), are best suited for small files that 
compress especially well, such as textual data.

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

    Script example:

      $ python3 bsky_post.py --handle exampleuser.bsky.social --password pxae-f17r-alp4-xqka 
          --image jrif_11050.jpg --alt-text "alt-text to describe image..." 
          "text to appear in main post..."

    You also need to create an app password for your Bluesky account: https://bsky.app/settings/app-passwords

    Bluesky size limits: Cover 800 KB / Secret data file (compressed) ~171 KB

  -r (Reddit) : Creates compatible “file-embedded” JPG images for posting on Reddit.

      $ jdvrif conceal -r my_image.jpg secret.mp3

    From the Reddit site, click “Create Post”, then select the “Images & Video” tab to attach the JPG image.
    These images are only compatible for posting on Reddit.

──────────────────────────
Notes
──────────────────────────

• To correctly download images from X-Twitter or Reddit, click image within the post to fully expand it before saving.
• ImgPile: sign in to an account before sharing; otherwise, the embedded data will not be preserved.

)"; 
}

enum class Mode : unsigned char { conceal, recover };
enum class Option : unsigned char { None, Bluesky, Reddit };

struct ProgramArgs {
	Mode mode{Mode::conceal};
	Option option{Option::None};
	fs::path image_file_path;
	fs::path data_file_path;
    
	static ProgramArgs parse(int argc, char** argv) {
		using std::string_view;

        auto arg = [&](int i) -> string_view {
			return (i >= 0 && i < argc) ? string_view(argv[i]) : string_view{};
        };

        const std::string prog = fs::path(argv[0]).filename().string();
        const std::string USAGE =
        	"Usage: " + prog + " conceal [-b|-r] <cover_image> <secret_file>\n\t\b"
            + prog + " recover <cover_image>\n\t\b"
            + prog + " --info";

        auto die = [&]() -> void {
        	throw std::runtime_error(USAGE);
        };

        if (argc < 2) die();

        if (argc == 2 && arg(1) == "--info") {
        	displayInfo();
        	std::exit(0);
        }

        ProgramArgs out{};

        const string_view cmd = arg(1);

        if (cmd == "conceal") {
        	int i = 2;

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

        if (cmd == "recover") {
        	if (argc != 3) die();
        	out.image_file_path = fs::path(arg(2));
        	out.mode = Mode::recover;
        	return out;
        }

        die();
        return out; // Keeps compiler happy.
    }
};

// Return vector index location for relevant signature search.
template <typename T, size_t N>
static inline uint32_t searchSig(std::vector<uint8_t>& vec, const std::array<T, N>& SIG) {
	return static_cast<uint32_t>(std::search(vec.begin(), vec.end(), SIG.begin(), SIG.end()) - vec.begin());
}

// Writes updated values (2, 4 or 8 bytes), such as segments lengths, index/offsets values, etc. into the relevant vector index location.	
static inline void updateValue(std::vector<uint8_t>& vec, uint32_t insert_index, uint64_t NEW_VALUE, uint8_t bits) {
	while (bits) {
		vec[insert_index++] = (NEW_VALUE >> (bits -= 8)) & 0xFF; // Big-endian.
    }
}

static inline bool hasValidFilename(const fs::path& p) {
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

static inline bool hasFileExtension(const fs::path& p, std::initializer_list<const char*> exts) {
	auto e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    for (const char* cand : exts) {
    	std::string c = cand;
        std::transform(c.begin(), c.end(), c.begin(), [](unsigned char x){ return static_cast<char>(std::tolower(x)); });
        if (e == c) return true;
    }
    return false;
}

// Zlib function, deflate or inflate data file within vector.
static inline void zlibFunc(std::vector<uint8_t>& vec, Mode mode, bool& isCompressedFile) {
	constexpr uint32_t BUFSIZE = 2 * 1024 * 1024; 
	const uint32_t VEC_SIZE = static_cast<uint32_t>(vec.size());
	
    static std::vector<uint8_t> buffer_vec(BUFSIZE);
    static std::vector<uint8_t> tmp_vec;
    tmp_vec.reserve(VEC_SIZE + BUFSIZE);

    z_stream strm{};
    strm.next_in   = vec.data();
    strm.avail_in  = VEC_SIZE;
    strm.next_out  = buffer_vec.data();
    strm.avail_out = BUFSIZE;

    if (mode == Mode::conceal) {
    	auto select_compression_level = [](uint32_t vec_size, bool isCompressedFile) -> int {
    		constexpr uint32_t 
    			FIFTH_SIZE_OPTION   = 750 * 1024 * 1024,
    			FOURTH_SIZE_OPTION  = 450 * 1024 * 1024,
    			THIRD_SIZE_OPTION   = 250 * 1024 * 1024,
    			SECOND_SIZE_OPTION  = 150 * 1024 * 1024,
    			FIRST_SIZE_OPTION   = 10  * 1024 * 1024;
    					
    		if (isCompressedFile || vec_size >= FIFTH_SIZE_OPTION) return Z_NO_COMPRESSION;
    		if (vec_size >= FOURTH_SIZE_OPTION) return Z_BEST_SPEED;
    		if (vec_size >= THIRD_SIZE_OPTION)  return Z_DEFAULT_COMPRESSION;
    		if (vec_size >= SECOND_SIZE_OPTION) return Z_BEST_COMPRESSION;
    		if (vec_size >= FIRST_SIZE_OPTION)  return Z_DEFAULT_COMPRESSION;
    		return Z_BEST_COMPRESSION;
		};
        
        int compression_level = select_compression_level(VEC_SIZE, isCompressedFile);

        if (deflateInit(&strm, compression_level)  != Z_OK) {
        	throw std::runtime_error("Zlib Deflate Init Error");
        }
        		
        while (strm.avail_in > 0) {
        	int ret = deflate(&strm, Z_NO_FLUSH);
            if (ret != Z_OK) {
            	deflateEnd(&strm);
                throw std::runtime_error("Zlib Compression Error");
            }
            if (strm.avail_out == 0) {
            	tmp_vec.insert(tmp_vec.end(), buffer_vec.begin(), buffer_vec.end());
                strm.next_out = buffer_vec.data();
                strm.avail_out = BUFSIZE;
            }
        }
        int ret;
        do {
			ret = deflate(&strm, Z_FINISH);
            size_t bytes_written = BUFSIZE - strm.avail_out;
            if (bytes_written > 0) {
            	tmp_vec.insert(tmp_vec.end(), buffer_vec.begin(), buffer_vec.begin() + bytes_written);
            }
            strm.next_out = buffer_vec.data();
            strm.avail_out = BUFSIZE;
        } while (ret == Z_OK);
        		
        deflateEnd(&strm);
        		
    } else { 
		// (Inflate)
        if (inflateInit(&strm) != Z_OK) {
        	throw std::runtime_error("Zlib Inflate Init Error");
        }
        while (strm.avail_in > 0) {
        	int ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_END) {
            	size_t bytes_written = BUFSIZE - strm.avail_out;
                if (bytes_written > 0) {
                	tmp_vec.insert(tmp_vec.end(), buffer_vec.begin(), buffer_vec.begin() + bytes_written);
                }
                inflateEnd(&strm);
                goto inflate_done; 
            }
            if (ret != Z_OK) {
            	inflateEnd(&strm);
                throw std::runtime_error("Zlib Inflate Error: " + std::string(strm.msg ? strm.msg : "Unknown error"));
            }
            if (strm.avail_out == 0) {
            	tmp_vec.insert(tmp_vec.end(), buffer_vec.begin(), buffer_vec.end());
                strm.next_out = buffer_vec.data();
                strm.avail_out = BUFSIZE;
            }
        }

        {	
        int ret;
        do {
        	ret = inflate(&strm, Z_FINISH);
            	size_t bytes_written = BUFSIZE - strm.avail_out;
                if (bytes_written > 0) {
                	tmp_vec.insert(tmp_vec.end(), buffer_vec.begin(), buffer_vec.begin() + bytes_written);
                }
                strm.next_out = buffer_vec.data();
                strm.avail_out = BUFSIZE;
            } while (ret == Z_OK);
        }
        inflateEnd(&strm);
    }
	inflate_done:
    	vec.swap(tmp_vec);
    	std::vector<uint8_t>().swap(tmp_vec);
    	std::vector<uint8_t>().swap(buffer_vec);
}

int main(int argc, char** argv) {
	try {
		#ifdef _WIN32
    		SetConsoleOutputCP(CP_UTF8);  
		#endif
		
		ProgramArgs args = ProgramArgs::parse(argc, argv);
				
		bool isCompressedFile = false;
			
		if (!fs::exists(args.image_file_path)) {
        	throw std::runtime_error("Image File Error: File not found.");
    	}
			
		if (!hasValidFilename(args.image_file_path)) {
    		throw std::runtime_error("Invalid Input Error: Unsupported characters in filename arguments.");
		}

		if (!hasFileExtension(args.image_file_path, {".jpg", ".jpeg", ".jfif"})) {
        	throw std::runtime_error("File Type Error: Invalid image extension. Only expecting \".jpg\", \".jpeg\", or \".jfif\".");
    	}
    			
		std::ifstream image_file_ifs(args.image_file_path, std::ios::binary);
        	
    	if (!image_file_ifs) {
    		throw std::runtime_error("Read File Error: Unable to read image file. Check the filename and try again.");
   		}

		uintmax_t image_file_size = fs::file_size(args.image_file_path);

    	constexpr uint8_t MINIMUM_IMAGE_SIZE = 134;

    	if (MINIMUM_IMAGE_SIZE > image_file_size) {
        	throw std::runtime_error("Image File Error: Invalid file size.");
    	}
			
		constexpr uintmax_t 
			MAX_IMAGE_SIZE_BLUESKY 	= 800ULL * 1024,
			MAX_SIZE_RECOVER 	= 3ULL * 1024 * 1024 * 1024;    
	
		if (args.option == Option::Bluesky && image_file_size > MAX_IMAGE_SIZE_BLUESKY) {
			throw std::runtime_error("File Size Error: Image file exceeds maximum size limit for the Bluesky platform.");
		}

		if (args.mode == Mode::recover && image_file_size > MAX_SIZE_RECOVER) {
			throw std::runtime_error("File Size Error: Image file exceeds maximum default size limit for jdvrif.");
		}
			
		std::vector<uint8_t> image_file_vec(image_file_size);
	
		image_file_ifs.read(reinterpret_cast<char*>(image_file_vec.data()), image_file_size);
		image_file_ifs.close();
	
		constexpr std::array<uint8_t, 2>
			IMAGE_START_SIG	{ 0xFF, 0xD8 },
			IMAGE_END_SIG   { 0xFF, 0xD9 };

		if (!std::equal(IMAGE_START_SIG.begin(), IMAGE_START_SIG.end(), image_file_vec.begin()) || !std::equal(IMAGE_END_SIG.begin(), IMAGE_END_SIG.end(), image_file_vec.end() - 2)) {
    		throw std::runtime_error("Image File Error: This is not a valid JPG image.");
		}
		
        constexpr uint32_t LARGE_FILE_SIZE = 300 * 1024 * 1024;
        const std::string LARGE_FILE_MSG = "\nPlease wait. Larger files will take longer to complete this process.\n";
        	
        if (args.mode == Mode::conceal) {                                    
			// Embed data file section code.
			std::vector<std::string> platforms_vec { 
				"X-Twitter", "Tumblr", 
				"Bluesky. (Only share this \"file-embedded\" JPG image on Bluesky).\n\n You must use the Python script \"bsky_post.py\" (found in the repo src folder)\n to post the image to Bluesky.", 
				"Mastodon", "Reddit. (Only share this \"file-embedded\" JPG image on Reddit).",
				"PostImage", "ImgBB", "ImgPile",  "Flickr", 
			};
				
			// To improve compatibility, default re-encode image to JPG Progressive format with a quality value set at 97 with no chroma subsampling.
			// If Bluesky option, re-encode to standard Baseline format with a quality value set at 85.
			// -------------
			tjhandle decompressor = tjInitDecompress();
    		if (!decompressor) {
        		throw std::runtime_error("tjInitDecompress() failed.");
    		}

    		int width = 0, height = 0, jpegSubsamp = 0, jpegColorspace = 0;
    		if (tjDecompressHeader3(decompressor, image_file_vec.data(), static_cast<unsigned long>(image_file_vec.size()), &width, &height, &jpegSubsamp, &jpegColorspace) != 0) {
        		tjDestroy(decompressor);
        		throw std::runtime_error(std::string("tjDecompressHeader3: ") + tjGetErrorStr());
    		}

    		std::vector<uint8_t> decoded_image_vec(width * height * 3); 
    		if (tjDecompress2(decompressor, image_file_vec.data(),static_cast<unsigned long>(image_file_vec.size()), decoded_image_vec.data(), width, 0, height, TJPF_RGB, 0) != 0) {
        		tjDestroy(decompressor);
        		throw std::runtime_error(std::string("tjDecompress2: ") + tjGetErrorStr());
    		}
    		tjDestroy(decompressor);
    		tjhandle compressor = tjInitCompress();
    		if (!compressor) {
        		throw std::runtime_error("tjInitCompress() failed.");
    		}

    		const uint8_t JPG_QUALITY_VAL = (args.option == Option::Bluesky) ? 85 : 97;

    		uint8_t* jpegBuf = nullptr;
    		unsigned long jpegSize = 0;

    		int flags = TJFLAG_ACCURATEDCT | ((args.option == Option::Bluesky) ? 0 : TJFLAG_PROGRESSIVE);

    		if (tjCompress2(compressor, decoded_image_vec.data(), width, 0, height, TJPF_RGB, &jpegBuf, &jpegSize, TJSAMP_444, JPG_QUALITY_VAL, flags) != 0) {
        		tjDestroy(compressor);
        		throw std::runtime_error(std::string("tjCompress2: ") + tjGetErrorStr());
    		}
    		tjDestroy(compressor);

    		std::vector<uint8_t> output_image_vec(jpegBuf, jpegBuf + jpegSize);
    		tjFree(jpegBuf);
    			
    		image_file_vec.swap(output_image_vec);
    			
    		std::vector<uint8_t>().swap(output_image_vec);
    		std::vector<uint8_t>().swap(decoded_image_vec);	
			// ------------
	
			// Remove superfluous segments from cover image. (EXIF, ICC color profile, etc).
			constexpr std::array<uint8_t, 2>
				APP1_SIG { 0xFF, 0xE1 }, // EXIF SEGMENT MARKER.
				APP2_SIG { 0xFF, 0xE2 }; // ICC COLOR PROFILE SEGMENT MARKER.

			constexpr std::array<uint8_t, 4>
				DQT1_SIG { 0xFF, 0xDB, 0x00, 0x43 },
				DQT2_SIG { 0xFF, 0xDB, 0x00, 0x84 };
		
			const uint32_t APP1_POS = searchSig(image_file_vec, APP1_SIG);

			if (image_file_vec.size() > APP1_POS) {
				const uint16_t APP1_BLOCK_SIZE = (static_cast<uint16_t>(image_file_vec[APP1_POS + 2]) << 8) | static_cast<uint16_t>(image_file_vec[APP1_POS + 3]);
				image_file_vec.erase(image_file_vec.begin() + APP1_POS, image_file_vec.begin() + APP1_POS + APP1_BLOCK_SIZE + 2);
			}

			const uint32_t APP2_POS = searchSig(image_file_vec, APP2_SIG);
			
			if (image_file_vec.size() > APP2_POS) {
				const uint16_t APP2_BLOCK_SIZE = (static_cast<uint16_t>(image_file_vec[APP2_POS + 2]) << 8) | static_cast<uint16_t>(image_file_vec[APP2_POS + 3]);
				image_file_vec.erase(image_file_vec.begin() + APP2_POS, image_file_vec.begin() + APP2_POS + APP2_BLOCK_SIZE + 2);
			}

			const uint32_t
				DQT1_POS = searchSig(image_file_vec, DQT1_SIG),
				DQT2_POS = searchSig(image_file_vec, DQT2_SIG),
				DQT_POS  = std::min(DQT1_POS, DQT2_POS);

			image_file_vec.erase(image_file_vec.begin(), image_file_vec.begin() + DQT_POS);
				// ------------

			image_file_size = image_file_vec.size();  // Get updated image size after image re-encode and removing superfluous segments.
			
			if (!hasValidFilename(args.data_file_path)) {
				throw std::runtime_error("Invalid Input Error: Unsupported characters in filename arguments.");
    		}
	
			if (!fs::exists(args.data_file_path) || !fs::is_regular_file(args.data_file_path)) {
        		throw std::runtime_error("Data File Error: File not found or not a regular file.");
    		}
    				
    		std::ifstream data_file_ifs(args.data_file_path, std::ios::binary);

			if (!data_file_ifs) {
				throw std::runtime_error("Read File Error: Unable to read data file. Check the filename and try again.");
			}
	
			constexpr uint8_t DATA_FILENAME_MAX_LENGTH = 20;

			std::string data_filename = args.data_file_path.filename().string();

			if (data_filename.size() > DATA_FILENAME_MAX_LENGTH) {
    			throw std::runtime_error("Data File Error: For compatibility requirements, length of data filename must not exceed 20 characters.");
			}
		
			uintmax_t data_file_size = fs::file_size(args.data_file_path);	
			
    		if (!data_file_size) {
				throw std::runtime_error("Data File Error: File is empty.");
    		}
    				
    		isCompressedFile = hasFileExtension(args.data_file_path, {
        		".zip",".jar",".rar",".7z",".bz2",".gz",".xz",".tar",
        		".lz",".lz4",".cab",".rpm",".deb",
        		".mp4",".mp3",".exe",".jpg",".jpeg",".jfif",".png",".webp",".bmp",".gif",
        		".ogg",".flac"
    		});
    				
			constexpr uintmax_t 
				MAX_DATA_SIZE_BLUESKY 	= 5ULL * 1024 * 1024,
				MAX_SIZE_CONCEAL 		= 2ULL * 1024 * 1024 * 1024,  		
    			MAX_SIZE_REDDIT 		= 20ULL * 1024 * 1024;         

			const uintmax_t COMBINED_FILE_SIZE = data_file_size + image_file_size;

			if (args.option == Option::Bluesky && data_file_size > MAX_DATA_SIZE_BLUESKY) {
				throw std::runtime_error("Data File Size Error: File exceeds maximum size limit for the Bluesky platform.");
			}

   			if (args.option == Option::Reddit && COMBINED_FILE_SIZE > MAX_SIZE_REDDIT) {
   				throw std::runtime_error("File Size Error: Combined size of image and data file exceeds maximum size limit for the Reddit platform.");
   			}

			if (args.option == Option::None && COMBINED_FILE_SIZE > MAX_SIZE_CONCEAL) {
				throw std::runtime_error("File Size Error: Combined size of image and data file exceeds maximum default size limit for jdvrif.");
			}
	
			std::vector<uint8_t> data_file_vec(data_file_size);

			data_file_ifs.read(reinterpret_cast<char*>(data_file_vec.data()), data_file_size);
			data_file_ifs.close();
								
			// ICC color profile segment (FFE2). Default method for storing data file (in multiple segments, if required).
			// Notes: 	Total segments value index = 0x2E0 (2 bytes)
			//		Compressed data file size index = 0x2E2	(4 bytes)
			//		Data filename length index = 0x2E6 (1 byte)
			//		Data filename index = 0x2E7 (20 bytes)
			//		Data filename XOR key index = 0x2FB (24 bytes)
			//		Sodium key index = 0x313 (32 bytes)
			//		Nonce key index = 0x333 (24 bytes)
			//		jdvrif sig index = 0x34B (7 bytes)
			//		Data file start index = 0x353 (see index 0x2E2 (4 bytes) for compressed data file size).

			std::vector<uint8_t>segment_vec {
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
			//		Compressed data file size index = 0x1CD	(4 bytes)
			//		Data filename length index = 0x160 (1 byte)
			//		Data filename index = 0x161 (20 bytes)
			//		Data filename XOR key index = 0x175 (24 bytes)
			//		Sodium key index = 0x18D (32 bytes)
			//		Nonce key index = 0x1AD (24 bytes)
			//		jdvrif sig index = 0x1C5 (7 bytes)
			//		Data file start index = 0x1D1 (see index 0x1CD (4 bytes) for compressed data file size).

			std::vector<uint8_t>bluesky_exif_vec {
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
				
			if (args.option == Option::Bluesky) {
				// Use the EXIF segment instead of the default color profile segment to store user data.
				// The color profile segment (FFE2) is removed by Bluesky, so we use EXIF.
				segment_vec.swap(bluesky_exif_vec);	
				std::vector<uint8_t>().swap(bluesky_exif_vec);
			} else {
				std::vector<uint8_t>().swap(bluesky_exif_vec);
			}
			
			const uint16_t DATA_FILENAME_LENGTH_INDEX = (args.option == Option::Bluesky) ? 0x160 : 0x2E6;

			segment_vec[DATA_FILENAME_LENGTH_INDEX] = static_cast<uint8_t>(data_filename.size());	 

			if (data_file_size > LARGE_FILE_SIZE) {
				std::cout << LARGE_FILE_MSG;
			}
			
			// Deflate data file with Zlib.
			zlibFunc(data_file_vec, args.mode, isCompressedFile);
			
			// Encrypt data file using the Libsodium cryptographic library
			std::random_device rd;
 			std::mt19937 gen(rd());
			std::uniform_int_distribution<unsigned short> dis(1, 255); 
		
			constexpr uint8_t XOR_KEY_LENGTH = 24;
	
			uint16_t
				data_filename_xor_key_index = (args.option == Option::Bluesky) ? 0x175 : 0x2FB,
				data_filename_index = (args.option == Option::Bluesky) ? 0x161: 0x2E7;
		
			uint8_t
				value_bit_length = 32, 
				data_filename_length = segment_vec[data_filename_index - 1],
				data_filename_char_pos = 0;

			std::generate_n(segment_vec.begin() + data_filename_xor_key_index, XOR_KEY_LENGTH, [&dis, &gen]() { return static_cast<uint8_t>(dis(gen)); });

			std::transform(
        		data_filename.begin() + data_filename_char_pos, data_filename.begin() + data_filename_char_pos + data_filename_length,
        		segment_vec.begin() + data_filename_xor_key_index, segment_vec.begin() + data_filename_index,
        		[](char a, uint8_t b) { return static_cast<uint8_t>(a) ^ b; }
    		);	
	
			const uint32_t DATA_FILE_VEC_SIZE = static_cast<uint32_t>(data_file_vec.size());

			segment_vec.reserve(segment_vec.size() + DATA_FILE_VEC_SIZE);
	
			static std::array<uint8_t, crypto_secretbox_KEYBYTES> key;
    		crypto_secretbox_keygen(key.data());

			static std::array<uint8_t, crypto_secretbox_NONCEBYTES> nonce;
   			randombytes_buf(nonce.data(), nonce.size());

			constexpr uint16_t EXIF_SEGMENT_DATA_INSERT_INDEX = 0x1D1;

			const uint16_t
				SODIUM_KEY_INDEX = (args.option == Option::Bluesky) ? 0x18D : 0x313,     
				NONCE_KEY_INDEX  = (args.option == Option::Bluesky) ? 0x1AD : 0x333;  
	
			std::copy_n(key.begin(), crypto_secretbox_KEYBYTES, segment_vec.begin() + SODIUM_KEY_INDEX); 	
			std::copy_n(nonce.begin(), crypto_secretbox_NONCEBYTES, segment_vec.begin() + NONCE_KEY_INDEX);

    		std::vector<uint8_t> encrypted_vec(DATA_FILE_VEC_SIZE + crypto_secretbox_MACBYTES); 

    		crypto_secretbox_easy(encrypted_vec.data(), data_file_vec.data(), DATA_FILE_VEC_SIZE, nonce.data(), key.data());
    		std::vector<uint8_t>().swap(data_file_vec);

			std::vector<uint8_t> 
				bluesky_xmp_vec,
				bluesky_pshop_vec;
			
			if (args.option == Option::Bluesky) { 
				// User has selected the -b argument option for the Bluesky platform.
				// + With EXIF overhead segment data (511) - four bytes we don't count (FFD8 FFE1),  
				// = Max. segment size 65534 (0xFFFE). Can't have 65535 (0xFFFF) as Bluesky will strip the EXIF segment.
				constexpr uint16_t 
					EXIF_SEGMENT_DATA_SIZE_LIMIT = 65027,
					COMPRESSED_FILE_SIZE_INDEX   = 0x1CD;
					
				const uint32_t ENCRYPTED_VEC_SIZE = static_cast<uint32_t>(encrypted_vec.size());
		
				value_bit_length = 32;					 	 
		
				updateValue(segment_vec, COMPRESSED_FILE_SIZE_INDEX, ENCRYPTED_VEC_SIZE, value_bit_length);

				// Split the data file if it exceeds the Max. compressed EXIF capacity of ~64KB. 
				// We can use the Photoshop segment to store more data, again ~64KB Max. stored as two ~32KB datasets within the segment.
				// If the data file exceeds the Photoshop segement, we can then try and fit the remaining data in the XMP segment (Base64 encoded).
				// EXIF (~64KB) --> Photoshop (~64KB (2x ~32KB datasets)) --> XMP (~42KB (data encoded and stored as Base64)). Max. ~170KB.

				if (ENCRYPTED_VEC_SIZE > EXIF_SEGMENT_DATA_SIZE_LIMIT) {	
					segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, encrypted_vec.begin(), encrypted_vec.begin() + EXIF_SEGMENT_DATA_SIZE_LIMIT);

					uint32_t 
						remaining_data_size = ENCRYPTED_VEC_SIZE - EXIF_SEGMENT_DATA_SIZE_LIMIT,
						data_file_index = EXIF_SEGMENT_DATA_SIZE_LIMIT;
						
					constexpr uint16_t
						FIRST_DATASET_SIZE_LIMIT = 32767, // 0x7FFF
						LAST_DATASET_SIZE_LIMIT  = 32730; // 0x7FDA 
						
					constexpr uint8_t FIRST_DATASET_SIZE_INDEX = 0x21;
						
					value_bit_length = 16;
						
					bluesky_pshop_vec = { 
						0xFF, 0xED, 0x00, 0x21, 0x50, 0x68, 0x6F, 0x74, 0x6F, 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E, 0x30, 0x00, 0x38, 0x42, 0x49,
						0x4D, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x1C, 0x08, 0x0A, 0x00, 0x00
					};
						
					updateValue(bluesky_pshop_vec, FIRST_DATASET_SIZE_INDEX, (FIRST_DATASET_SIZE_LIMIT >= remaining_data_size ? remaining_data_size : FIRST_DATASET_SIZE_LIMIT), value_bit_length);
						
					std::copy_n(encrypted_vec.begin() + data_file_index, (FIRST_DATASET_SIZE_LIMIT >= remaining_data_size ? remaining_data_size : FIRST_DATASET_SIZE_LIMIT), std::back_inserter(bluesky_pshop_vec));
						
					if (remaining_data_size > FIRST_DATASET_SIZE_LIMIT) {	
						remaining_data_size -= FIRST_DATASET_SIZE_LIMIT;
						data_file_index += FIRST_DATASET_SIZE_LIMIT;
							
						// Add an additional (final) dataset to the bluesky_pshop_vec
						constexpr uint8_t DATASET_SIZE_INDEX = 3;
								
						std::vector<uint8_t> dataset_marker_vec { 0x1C, 0x08, 0x0A, 0x00, 0x00 }; // 3 byte dataset ID, 2 byte length field.
							
						updateValue(dataset_marker_vec, DATASET_SIZE_INDEX, (LAST_DATASET_SIZE_LIMIT >= remaining_data_size ? remaining_data_size : LAST_DATASET_SIZE_LIMIT), value_bit_length);
								
						std::copy_n(dataset_marker_vec.begin(), dataset_marker_vec.size(), std::back_inserter(bluesky_pshop_vec));		
							
						std::copy_n(encrypted_vec.begin() + data_file_index, (LAST_DATASET_SIZE_LIMIT >= remaining_data_size ? remaining_data_size : LAST_DATASET_SIZE_LIMIT), std::back_inserter(bluesky_pshop_vec));
							
						if (remaining_data_size > LAST_DATASET_SIZE_LIMIT) {	
							remaining_data_size -= LAST_DATASET_SIZE_LIMIT;
							data_file_index += LAST_DATASET_SIZE_LIMIT;
								
							std::vector<uint8_t> tmp_xmp_vec(remaining_data_size);
			
							std::copy_n(encrypted_vec.begin() + data_file_index, remaining_data_size, tmp_xmp_vec.begin());
			
							// We can only store Base64 encoded data in the XMP segment, so convert the binary data here.
							static constexpr char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    						uint32_t 
								input_size = static_cast<uint32_t>(tmp_xmp_vec.size()),
    							output_size = ((input_size + 2) / 3) * 4; 

    						std::vector<uint8_t> temp_vec(output_size); 

    						uint32_t j = 0;
    						for (uint32_t i = 0; i < input_size; i += 3) {
        						uint32_t 
									octet_a = tmp_xmp_vec[i],
        							octet_b = (i + 1 < input_size) ? tmp_xmp_vec[i + 1] : 0,
        							octet_c = (i + 2 < input_size) ? tmp_xmp_vec[i + 2] : 0,
        							triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        						temp_vec[j++] = base64_table[(triple >> 18) & 0x3F];
        						temp_vec[j++] = base64_table[(triple >> 12) & 0x3F];
        						temp_vec[j++] = (i + 1 < input_size) ? base64_table[(triple >> 6) & 0x3F] : '=';
        						temp_vec[j++] = (i + 2 < input_size) ? base64_table[triple & 0x3F] : '=';
    						}
    						tmp_xmp_vec.swap(temp_vec);
    						std::vector<uint8_t>().swap(temp_vec);
							// ------------
			
							constexpr uint16_t XMP_SEGMENT_DATA_INSERT_INDEX = 0x139;
							
							// XMP (FFE1) segment.
							// Notes: 	Data file index = 0x139 (Remainder part of data file stored here if too big for Photoshop segment (bluesky_pshop_vec). Data file content stored here as BASE64).
							
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
							bluesky_xmp_vec.insert(bluesky_xmp_vec.begin() + XMP_SEGMENT_DATA_INSERT_INDEX, tmp_xmp_vec.begin(), tmp_xmp_vec.end());

							std::vector<uint8_t>().swap(tmp_xmp_vec);		
						}
					}
				} else { 
					// Data file was small enough to fit within the EXIF segment, XMP segment not required.
					segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, encrypted_vec.begin(), encrypted_vec.end());
					std::vector<uint8_t>().swap(bluesky_xmp_vec);
				}
			} else { 
				// Used the default color profile segment for data storage.
				std::copy_n(encrypted_vec.begin(), encrypted_vec.size(), std::back_inserter(segment_vec));
			}	
	
			std::vector<uint8_t>().swap(encrypted_vec);
	
			uint64_t pin = 0;
	
			for (uint8_t i = 0; i < 8; ++i) {
        		pin = (pin << 8) | static_cast<uint64_t>(segment_vec[SODIUM_KEY_INDEX + i]);
    		}
	
			uint16_t 
				sodium_xor_key_pos = SODIUM_KEY_INDEX,
				sodium_key_pos = SODIUM_KEY_INDEX;

			uint8_t sodium_keys_length = 48;
	
			value_bit_length = 64;

			sodium_key_pos += 8; 

			constexpr uint8_t SODIUM_XOR_KEY_LENGTH = 8;  

			while (sodium_keys_length--) {   
    			segment_vec[sodium_key_pos] = segment_vec[sodium_key_pos] ^ segment_vec[sodium_xor_key_pos++];
				sodium_key_pos++;
    			sodium_xor_key_pos = (sodium_xor_key_pos >= SODIUM_XOR_KEY_LENGTH + SODIUM_KEY_INDEX) 
                	? SODIUM_KEY_INDEX 
                    : sodium_xor_key_pos;
			}
	
			sodium_key_pos = SODIUM_KEY_INDEX; 

			std::mt19937_64 gen64(rd()); 
    		std::uniform_int_distribution<uint64_t> dis64; 

    		const uint64_t RANDOM_VAL = dis64(gen64); 

			updateValue(segment_vec, sodium_key_pos, RANDOM_VAL, value_bit_length);
			// ------------
	
			value_bit_length = 16;

			if (args.option == Option::Bluesky) {	 // We can store binary data within the first (EXIF) segment, with a max compressed storage capacity close to ~64KB. See encryptFile.cpp
				constexpr uint8_t 
					MARKER_BYTES_VAL = 4, // FFD8, FFE1
					EXIF_SIZE_FIELD_INDEX = 0x04,  
					EXIF_XRES_OFFSET_FIELD_INDEX = 0x2A,  
					EXIF_YRES_OFFSET_FIELD_INDEX = 0x36,  
					EXIF_ARTIST_SIZE_FIELD_INDEX = 0x4A,  
					EXIF_SUBIFD_OFFSET_FIELD_INDEX = 0x5A;  

				const uint32_t EXIF_SEGMENT_SIZE = static_cast<uint32_t>(segment_vec.size() - MARKER_BYTES_VAL);

				const uint16_t	
					EXIF_XRES_OFFSET   = EXIF_SEGMENT_SIZE - 0x36,
					EXIF_YRES_OFFSET   = EXIF_SEGMENT_SIZE - 0x2E,
					EXIF_SUBIFD_OFFSET = EXIF_SEGMENT_SIZE - 0x26,
					EXIF_ARTIST_SIZE   = EXIF_SEGMENT_SIZE - 0x8C;

				updateValue(segment_vec, EXIF_SIZE_FIELD_INDEX , EXIF_SEGMENT_SIZE, value_bit_length);
		
				value_bit_length = 32;

				updateValue(segment_vec, EXIF_XRES_OFFSET_FIELD_INDEX, EXIF_XRES_OFFSET, value_bit_length);
				updateValue(segment_vec, EXIF_YRES_OFFSET_FIELD_INDEX, EXIF_YRES_OFFSET, value_bit_length);
				updateValue(segment_vec, EXIF_ARTIST_SIZE_FIELD_INDEX, EXIF_ARTIST_SIZE, value_bit_length); 
				updateValue(segment_vec, EXIF_SUBIFD_OFFSET_FIELD_INDEX, EXIF_SUBIFD_OFFSET, value_bit_length);
					
				constexpr uint8_t BLUESKY_PSHOP_VEC_DEFAULT_SIZE = 35;  // PSHOP segment size without user data.
					
				if (bluesky_pshop_vec.size() > BLUESKY_PSHOP_VEC_DEFAULT_SIZE) {
					// Data file was too big for the EXIF segment, so will spill over to the PSHOP vec.	
					constexpr uint8_t 
						PSHOP_VEC_SEGMENT_SIZE_INDEX 	= 0x02,
						PSHOP_VEC_BIM_SIZE_INDEX 		= 0x1C,
						PSHOP_VEC_BIM_SIZE_DIFF			= 28,	// Consistant size difference between PSHOP segment size and BIM size.	
						PSHOP_SEGMENT_MARKER_BYTES 		= 2;
						
					uint16_t bluesky_pshop_segment_size = static_cast<uint16_t>(bluesky_pshop_vec.size()) - PSHOP_SEGMENT_MARKER_BYTES;
						 
					value_bit_length = 16;	
						
					updateValue(bluesky_pshop_vec, PSHOP_VEC_SEGMENT_SIZE_INDEX, bluesky_pshop_segment_size, value_bit_length);
					updateValue(bluesky_pshop_vec, PSHOP_VEC_BIM_SIZE_INDEX, (bluesky_pshop_segment_size - PSHOP_VEC_BIM_SIZE_DIFF), value_bit_length);
								
					std::copy_n(bluesky_pshop_vec.begin(), bluesky_pshop_vec.size(), std::back_inserter(segment_vec));
					std::vector<uint8_t>().swap(bluesky_pshop_vec);
				}
					
				constexpr uint16_t BLUESKY_XMP_VEC_DEFAULT_SIZE = 405;  // XMP segment size without user data.
		
				const uint32_t BLUESKY_XMP_VEC_SIZE = static_cast<uint32_t>(bluesky_xmp_vec.size());

				// Are we using the second (XMP) segment?
				if (BLUESKY_XMP_VEC_SIZE > BLUESKY_XMP_VEC_DEFAULT_SIZE) {

					// Size includes segment SIG two bytes (don't count). Bluesky will strip XMP data segment greater than 60031 bytes (0xEA7F).
					// With the overhead of the XMP default segment data (405 bytes) and the Base64 encoding overhead (~33%),
					// The max compressed binary data storage in this segment is probably around ~40KB. 

 					constexpr uint16_t XMP_SEGMENT_SIZE_LIMIT = 60033;

					if (BLUESKY_XMP_VEC_SIZE > XMP_SEGMENT_SIZE_LIMIT) {
						throw std::runtime_error("File Size Error: Data file exceeds segment size limit for Bluesky.");
					}

					constexpr uint8_t 
						SIG_LENGTH = 2, // FFE1
						XMP_SIZE_FIELD_INDEX = 0x02;
						
					value_bit_length = 16;
					updateValue(bluesky_xmp_vec, XMP_SIZE_FIELD_INDEX, BLUESKY_XMP_VEC_SIZE - SIG_LENGTH, value_bit_length);
			
					// Even though the order of the split data file (if required, depending on file size) is EXIF --> PHOTOSHOP --> XMP,
					// for compatibility requirements, the order of Segments within the image file are: EXIF --> XMP --> PHOTOSHOP.
					// To rebuild the file, if it extends to all three Segments, we take data in the order of EXIF --> PHOTOSHOP --> XMP.
						
					constexpr std::array<uint8_t, 12> PSHOP_SEGMENT_SIG { 0x50, 0x68, 0x6F, 0x74, 0x6F, 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E };

					constexpr uint8_t XMP_INSERT_INDEX_DIFF = 4;
												
					const uint32_t XMP_INSERT_INDEX = searchSig(segment_vec, PSHOP_SEGMENT_SIG) - XMP_INSERT_INDEX_DIFF;
						
					segment_vec.insert(segment_vec.begin() + XMP_INSERT_INDEX, bluesky_xmp_vec.begin(), bluesky_xmp_vec.end());
						
					std::vector<uint8_t>().swap(bluesky_xmp_vec);
				}
				
				image_file_vec.insert(image_file_vec.begin(), segment_vec.begin(), segment_vec.end());
				platforms_vec[0] = std::move(platforms_vec[2]);
				platforms_vec.resize(1);
			} else {
				// Default segment_vec uses color profile segment (FFE2) to store data file. If required, split data file and use multiple segments for these larger files.
				constexpr uint8_t
					IMAGE_START_SIG_LENGTH	  = 2,
					ICC_SEGMENT_SIG_LENGTH	  = 2,
					ICC_SEGMENT_HEADER_LENGTH = 16;

				uint16_t icc_segment_data_size = 65519;  // Max. data for each segment (Not including header and signature bytes).

				uint32_t 
					icc_profile_with_data_file_vec_size = static_cast<uint32_t>(segment_vec.size()),
					max_first_segment_size = icc_segment_data_size + IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH;
		
				if (icc_profile_with_data_file_vec_size > max_first_segment_size) { 
					// Data file is too large for a single icc segment, so split data file in to multiple icc segments.
					constexpr uint8_t LIBSODIUM_MACBYTES = 16;
					// 16 byte authentication tag used by libsodium. Don't count these bytes as part of the data file, as they will be removed during the decryption process.
	
					icc_profile_with_data_file_vec_size -= LIBSODIUM_MACBYTES;

					uint16_t 
						icc_segments_required       = (icc_profile_with_data_file_vec_size / icc_segment_data_size) + 1, // Usually a remainder segment, so plus 1 here.
						icc_segment_remainder_size  = (icc_profile_with_data_file_vec_size % icc_segment_data_size) - (IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH),
						icc_segments_sequence_val   = 1;
			
					constexpr uint16_t ICC_SEGMENTS_TOTAL_VAL_INDEX = 0x2E0;  // The value stored here is used by jdvout when extracting the data file.
					updateValue(segment_vec, ICC_SEGMENTS_TOTAL_VAL_INDEX, !icc_segment_remainder_size ? --icc_segments_required : icc_segments_required, value_bit_length);

					constexpr uint8_t 
						ICC_SEGMENTS_SEQUENCE_VAL_INDEX = 0x11,
						ICC_SEGMENT_REMAINDER_SIZE_INDEX = 0x04;

					std::vector<uint8_t> icc_segment_header_vec { 
						segment_vec.begin(), segment_vec.begin() + IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH 
					};

					// Because of some duplicate data, erase the first 20 bytes of segment_vec because they will be replaced when splitting the data file.
    				segment_vec.erase(segment_vec.begin(), segment_vec.begin() + (IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH));

					data_file_vec.reserve(icc_profile_with_data_file_vec_size + (icc_segments_required * (ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH)));

					uint32_t byte_index = 0;

					while (icc_segments_required--) {	
						if (!icc_segments_required) {
							if (icc_segment_remainder_size) {
								icc_segment_data_size = icc_segment_remainder_size;	
			   					updateValue(icc_segment_header_vec, ICC_SEGMENT_REMAINDER_SIZE_INDEX, (icc_segment_remainder_size + ICC_SEGMENT_HEADER_LENGTH), value_bit_length);
							} else {
								break;
							}	 	
						}
						std::copy_n(icc_segment_header_vec.begin() + IMAGE_START_SIG_LENGTH, ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH, std::back_inserter(data_file_vec));
						std::copy_n(segment_vec.begin() + byte_index, icc_segment_data_size, std::back_inserter(data_file_vec));
						updateValue(icc_segment_header_vec, ICC_SEGMENTS_SEQUENCE_VAL_INDEX, ++icc_segments_sequence_val, value_bit_length);
						byte_index += icc_segment_data_size;
					}

					std::vector<uint8_t>().swap(segment_vec);
		
					// Insert the start of image sig bytes that were removed.
					data_file_vec.insert(data_file_vec.begin(), icc_segment_header_vec.begin(), icc_segment_header_vec.begin() + IMAGE_START_SIG_LENGTH);
					std::vector<uint8_t>().swap(icc_segment_header_vec);
				} else {  
					// Data file is small enough to fit within a single icc profile segment.
					constexpr uint8_t
						ICC_SEGMENT_HEADER_SIZE_INDEX 	= 0x04, 
						ICC_PROFILE_SIZE_INDEX  		= 0x16, 
						ICC_PROFILE_SIZE_DIFF   		= 16;
			
					const uint16_t 
						SEGMENT_SIZE 	 = icc_profile_with_data_file_vec_size - (IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH),
						ICC_SEGMENT_SIZE = SEGMENT_SIZE - ICC_PROFILE_SIZE_DIFF;

					updateValue(segment_vec, ICC_SEGMENT_HEADER_SIZE_INDEX, SEGMENT_SIZE, value_bit_length);
					updateValue(segment_vec, ICC_PROFILE_SIZE_INDEX, ICC_SEGMENT_SIZE, value_bit_length);
					
					data_file_vec.swap(segment_vec);
					std::vector<uint8_t>().swap(segment_vec);
				}
		
				value_bit_length = 32; 

				constexpr uint16_t 
					DEFLATED_DATA_FILE_SIZE_INDEX = 0x2E2,	// The size value stored here is used by jdvout when extracting the data file.
					ICC_PROFILE_DATA_SIZE = 851; // Color profile data size, not including user data file size.
	
				updateValue(data_file_vec, DEFLATED_DATA_FILE_SIZE_INDEX, static_cast<uint32_t>(data_file_vec.size()) - ICC_PROFILE_DATA_SIZE, value_bit_length);
				// -------
		
				image_file_vec.reserve(image_file_size + data_file_vec.size());	
					
				if (args.option == Option::Reddit) {
					static constexpr std::array<uint8_t, 2> IMAGE_START_SIG { 0xFF, 0xD8 };
					image_file_vec.insert(image_file_vec.begin(), IMAGE_START_SIG.begin(), IMAGE_START_SIG.end());
					image_file_vec.insert(image_file_vec.end() - 2, 8000, 0x23);
					image_file_vec.insert(image_file_vec.end() - 2, data_file_vec.begin() + 2, data_file_vec.end());
					platforms_vec[0] = std::move(platforms_vec[4]);
					platforms_vec.resize(1);
				} else {
					platforms_vec.erase(platforms_vec.begin() + 4); 
					platforms_vec.erase(platforms_vec.begin() + 2);
					image_file_vec.insert(image_file_vec.begin(), data_file_vec.begin(), data_file_vec.end());
				}
				std::vector<uint8_t>().swap(data_file_vec);
			}	
	
    		std::uniform_int_distribution<> dist(10000, 99999);  

			const std::string OUTPUT_FILENAME = "jrif_" + std::to_string(dist(gen)) + ".jpg";

			std::ofstream file_ofs(OUTPUT_FILENAME, std::ios::binary);

			if (!file_ofs) {
				throw std::runtime_error("Write File Error: Unable to write to file. Make sure you have WRITE permissions for this location.");
			}
	
			const uint32_t IMAGE_SIZE = static_cast<uint32_t>(image_file_vec.size());

			file_ofs.write(reinterpret_cast<const char*>(image_file_vec.data()), IMAGE_SIZE);
			file_ofs.close();
			
			if (args.option == Option::None) {
				constexpr uint32_t 
					FLICKR_MAX_IMAGE_SIZE 			= 200 * 1024 * 1024,
					IMGPILE_MAX_IMAGE_SIZE 			= 100 * 1024 * 1024,
					IMGBB_POSTIMAGE_MAX_IMAGE_SIZE 	= 32 * 1024 * 1024,
					MASTODON_MAX_IMAGE_SIZE 		= 16 * 1024 * 1024,
					TWITTER_MAX_IMAGE_SIZE 			= 5 * 1024 * 1024;
					
				constexpr uint16_t 
					TWITTER_MAX_DATA_SIZE 	= 10 * 1024,
					TUMBLR_MAX_DATA_SIZE 	=  64 * 1024 - 2;
					
				const uint16_t
					FIRST_SEGMENT_SIZE	= (image_file_vec[0x04] << 8) | image_file_vec[0x05],
					TOTAL_SEGMENTS 		= (image_file_vec[0x2E0] << 8) | image_file_vec[0x2E1];
				
				constexpr uint8_t MASTODON_MAX_SEGMENTS = 100;
				
				std::vector<std::string> filtered_platforms;

				for (const std::string& platform : platforms_vec) {
    				if (platform == "X-Twitter" && (FIRST_SEGMENT_SIZE > TWITTER_MAX_DATA_SIZE || IMAGE_SIZE > TWITTER_MAX_IMAGE_SIZE)) {
        				continue;
    				}
    				if (platform == "Tumblr" && (FIRST_SEGMENT_SIZE > TUMBLR_MAX_DATA_SIZE)) {
        				continue;
    				}
    				if (platform == "Mastodon" && (TOTAL_SEGMENTS > MASTODON_MAX_SEGMENTS || IMAGE_SIZE > MASTODON_MAX_IMAGE_SIZE)) {
        				continue;
    				}
    				if ((platform == "ImgBB" || platform == "PostImage") && (IMAGE_SIZE > IMGBB_POSTIMAGE_MAX_IMAGE_SIZE)) {
        				continue;
    				}
    				if (platform == "ImgPile" && IMAGE_SIZE > IMGPILE_MAX_IMAGE_SIZE) {
        				continue;
    				}
    				if (platform == "Flickr" && IMAGE_SIZE > FLICKR_MAX_IMAGE_SIZE) {
        				continue;
    				}
					filtered_platforms.push_back(platform);
				}
				if (filtered_platforms.empty()) {
    				filtered_platforms.push_back("\b\bUnknown!\n\n Due to the large file size of the output JPG image, I'm unaware of any\n compatible platforms that this image can be posted on. Local use only?");
				}
				platforms_vec.swap(filtered_platforms);
				std::vector<std::string>().swap(filtered_platforms);
			}
			std::cout << "\nPlatform compatibility for output image:-\n\n";
			
			for (const auto& s : platforms_vec) {
        		std::cout << " ✓ "<< s << '\n' ;
   		 	}	
   		 	
			std::vector<std::string>().swap(platforms_vec);
			std::vector<uint8_t>().swap(image_file_vec);
			
			std::cout << "\nSaved \"file-embedded\" JPG image: " << OUTPUT_FILENAME << " (" << IMAGE_SIZE << " bytes).\n";
	
			std::cout << "\nRecovery PIN: [***" << pin << "***]\n\nImportant: Keep your PIN safe, so that you can extract the hidden file.\n\nComplete!\n\n";
			return 0;
        } else {
			// Recover data file section code.
        	constexpr uint8_t 
				SIG_LENGTH = 7,
				INDEX_DIFF = 8;

			constexpr std::array<uint8_t, SIG_LENGTH>
				JDVRIF_SIG	{ 0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9 },
				ICC_PROFILE_SIG	{ 0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42 };
		
			const uint32_t 
				JDVRIF_SIG_INDEX	= searchSig(image_file_vec, JDVRIF_SIG),
				ICC_PROFILE_SIG_INDEX 	= searchSig(image_file_vec, ICC_PROFILE_SIG);

			if (JDVRIF_SIG_INDEX == image_file_vec.size()) {
				throw std::runtime_error("Image File Error: Signature check failure. This is not a valid jdvrif \"file-embedded\" image.");
			}
	
			uint8_t pin_attempts_val = image_file_vec[JDVRIF_SIG_INDEX + INDEX_DIFF - 1];

			bool hasBlueskyOption = true;
		
			if (ICC_PROFILE_SIG_INDEX != image_file_vec.size()) {
				image_file_vec.erase(image_file_vec.begin(), image_file_vec.begin() + (ICC_PROFILE_SIG_INDEX - INDEX_DIFF));
				hasBlueskyOption = false;
			}

			if (hasBlueskyOption) { // EXIF segment (FFE1) is being used. Check for PHOTOSHOP & XMP segments and their index locations.
				static constexpr std::array<uint8_t, SIG_LENGTH> 
					PSHOP_SIG 	{ 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E },
					XMP_SIG 	{ 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F, 0x2F },
					XMP_CREATOR_SIG { 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69 };

				const uint32_t PSHOP_SIG_INDEX = searchSig(image_file_vec, PSHOP_SIG);
					
				if (PSHOP_SIG_INDEX != image_file_vec.size()) { // Found Photoshop segment.
					constexpr uint16_t MAX_SINGLE_DATASET_PSHOP_SEGMENT_SIZE = 32800; // If the photoshop segment size is greater than this size, we have two datasets.

					constexpr uint8_t 
						PSHOP_SEGMENT_SIZE_INDEX_DIFF = 7,
						PSHOP_FIRST_DATASET_SIZE_INDEX_DIFF = 24,
						PSHOP_DATASET_FILE_INDEX_DIFF = 2;
							
					const uint32_t 
						PSHOP_SEGMENT_SIZE_INDEX = PSHOP_SIG_INDEX - PSHOP_SEGMENT_SIZE_INDEX_DIFF,	
						PSHOP_FIRST_DATASET_SIZE_INDEX = PSHOP_SIG_INDEX + PSHOP_FIRST_DATASET_SIZE_INDEX_DIFF,
						PSHOP_FIRST_DATASET_FILE_INDEX = PSHOP_FIRST_DATASET_SIZE_INDEX + PSHOP_DATASET_FILE_INDEX_DIFF,
						PSHOP_SEGMENT_SIZE = (static_cast<uint16_t>(image_file_vec[PSHOP_SEGMENT_SIZE_INDEX]) << 8) | static_cast<uint16_t>(image_file_vec[PSHOP_SEGMENT_SIZE_INDEX + 1]),
						PSHOP_FIRST_DATASET_SIZE = (static_cast<uint16_t>(image_file_vec[PSHOP_FIRST_DATASET_SIZE_INDEX]) << 8) | static_cast<uint16_t>(image_file_vec[PSHOP_FIRST_DATASET_SIZE_INDEX + 1]);
											
					uint8_t	end_of_exif_data_index_diff = 55;
						
					uint32_t end_of_exif_data_index = PSHOP_SIG_INDEX - end_of_exif_data_index_diff;
											
					if (MAX_SINGLE_DATASET_PSHOP_SEGMENT_SIZE >= PSHOP_SEGMENT_SIZE) {
						// Just a single dataset.
						std::copy_n(image_file_vec.begin() + PSHOP_FIRST_DATASET_FILE_INDEX, PSHOP_FIRST_DATASET_SIZE, image_file_vec.begin() + end_of_exif_data_index);
					} else {
						// We have a second dataset for the photoshop segment.
						std::vector<uint8_t> pshop_tmp_vec;
						pshop_tmp_vec.reserve(PSHOP_FIRST_DATASET_SIZE);
							
						std::copy_n(image_file_vec.begin() + PSHOP_FIRST_DATASET_FILE_INDEX, PSHOP_FIRST_DATASET_SIZE, std::back_inserter(pshop_tmp_vec));
						
						constexpr uint8_t PSHOP_LAST_DATASET_SIZE_INDEX_DIFF = 3;
							
						const uint32_t 
							PSHOP_LAST_DATASET_SIZE_INDEX = PSHOP_FIRST_DATASET_FILE_INDEX + PSHOP_FIRST_DATASET_SIZE + PSHOP_LAST_DATASET_SIZE_INDEX_DIFF,
							PSHOP_LAST_DATASET_SIZE = (static_cast<uint16_t>(image_file_vec[PSHOP_LAST_DATASET_SIZE_INDEX]) << 8) | static_cast<uint16_t>(image_file_vec[PSHOP_LAST_DATASET_SIZE_INDEX + 1]),
							PSHOP_LAST_DATASET_FILE_INDEX = PSHOP_LAST_DATASET_SIZE_INDEX + PSHOP_DATASET_FILE_INDEX_DIFF;
								
						pshop_tmp_vec.reserve(pshop_tmp_vec.size() + PSHOP_LAST_DATASET_SIZE);
								
						std::copy_n(image_file_vec.begin() + PSHOP_LAST_DATASET_FILE_INDEX, PSHOP_LAST_DATASET_SIZE, std::back_inserter(pshop_tmp_vec));
							
						const uint32_t XMP_SIG_INDEX = searchSig(image_file_vec, XMP_SIG);
							
						if (XMP_SIG_INDEX == image_file_vec.size()) {
							std::copy_n(pshop_tmp_vec.begin(), pshop_tmp_vec.size(), image_file_vec.begin() + end_of_exif_data_index);
							std::vector<uint8_t>().swap(pshop_tmp_vec);
						} else { 
							// Found XMP segment.
							const uint32_t 
								XMP_CREATOR_SIG_INDEX = searchSig(image_file_vec, XMP_CREATOR_SIG),
								BEGIN_BASE64_DATA_INDEX = XMP_CREATOR_SIG_INDEX + SIG_LENGTH + 1;
			
							constexpr uint8_t END_BASE64_DATA_SIG = 0x3C;
								
							const uint32_t 
								END_BASE64_DATA_SIG_INDEX = static_cast<uint32_t>(std::find(image_file_vec.begin() + BEGIN_BASE64_DATA_INDEX, 
									image_file_vec.end(), END_BASE64_DATA_SIG) - image_file_vec.begin()),
										BASE64_DATA_SIZE = END_BASE64_DATA_SIG_INDEX - BEGIN_BASE64_DATA_INDEX;
	
							std::vector<uint8_t> base64_data_vec(BASE64_DATA_SIZE);
							std::copy_n(image_file_vec.begin() + BEGIN_BASE64_DATA_INDEX, BASE64_DATA_SIZE, base64_data_vec.begin());

							uint32_t input_size = static_cast<uint32_t>(base64_data_vec.size());
							
							if (input_size == 0 || (input_size % 4) != 0) {
    							throw std::invalid_argument("Base64 input size must be a multiple of 4 and non-empty");
							}

							const auto& in = base64_data_vec;

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
							
							auto decode_val = [&](uint8_t c) -> int {
    							return base64_table[c];
							};

							uint32_t padding = 0;
							
							if (in[input_size - 1] == '=') ++padding;
							if (in[input_size - 2] == '=') ++padding;

							uint32_t output_size = (input_size / 4) * 3 - padding;
							std::vector<uint8_t> output_vec(output_size);

							uint32_t o = 0;
							for (uint32_t i = 0; i < input_size; i += 4) {
    							const uint8_t 
									c0 = in[i + 0],
    								c1 = in[i + 1],
    								c2 = in[i + 2],
    								c3 = in[i + 3];

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

    							const uint32_t triple = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12) | (static_cast<uint32_t>(v2) << 6) |  static_cast<uint32_t>(v3);

    							output_vec[o++] = static_cast<uint8_t>((triple >> 16) & 0xFF);
								
    							if (!p2) output_vec[o++] = static_cast<uint8_t>((triple >> 8) & 0xFF);
    							if (!p3) output_vec[o++] = static_cast<uint8_t>(triple & 0xFF);
							}								
    						base64_data_vec.swap(output_vec);
    						std::vector<uint8_t>().swap(output_vec);
    						// ------------
								
							pshop_tmp_vec.reserve(pshop_tmp_vec.size() + base64_data_vec.size());
									
							std::copy_n(base64_data_vec.begin(), base64_data_vec.size(), std::back_inserter(pshop_tmp_vec));
							std::vector<uint8_t>().swap(base64_data_vec);
								
							end_of_exif_data_index_diff = 50;
									
							end_of_exif_data_index = XMP_SIG_INDEX - end_of_exif_data_index_diff;

							// Now append the binary data from the multiple segments to the EXIF binary segment data, so that we have the complete data file.
							std::copy_n(pshop_tmp_vec.begin(), pshop_tmp_vec.size(), image_file_vec.begin() + end_of_exif_data_index);
							std::vector<uint8_t>().swap(pshop_tmp_vec);
						}
					}
				}
			}
		
			if (image_file_size > LARGE_FILE_SIZE) {
				std::cout << LARGE_FILE_MSG;
			}
	
			// Decrypt embedded data file using the Libsodium cryptographic library.
			const uint16_t 
				SODIUM_KEY_INDEX = hasBlueskyOption ? 0x18D : 0x2FB,
				NONCE_KEY_INDEX =  hasBlueskyOption ? 0x1AD : 0x31B;

			uint16_t 
				sodium_key_pos = SODIUM_KEY_INDEX,
				sodium_xor_key_pos = SODIUM_KEY_INDEX;

			uint8_t
				sodium_keys_length = 48,
				value_bit_length = 64;
			
			bool hasDecryptionFailed = false;
		
			std::cout << "\nPIN: ";
	
			// Get recovery PIN from user input
			const std::string MAX_UINT64_STR = "18446744073709551615";
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
	
    		uint64_t recovery_pin;
    	
    		if (input.empty() || (input.length() == 20 && input > MAX_UINT64_STR)) {
        		recovery_pin = 0; 
    		} else {
        		recovery_pin = std::stoull(input); 
    		}
			// -----------

			updateValue(image_file_vec, sodium_key_pos, recovery_pin, value_bit_length); 	
		
			constexpr uint8_t SODIUM_XOR_KEY_LENGTH	= 8; 

			sodium_key_pos += SODIUM_XOR_KEY_LENGTH;

			while(sodium_keys_length--) {
				image_file_vec[sodium_key_pos] = image_file_vec[sodium_key_pos] ^ image_file_vec[sodium_xor_key_pos++];
				sodium_key_pos++;
				sodium_xor_key_pos = (sodium_xor_key_pos >= SODIUM_XOR_KEY_LENGTH + SODIUM_KEY_INDEX) 
					? SODIUM_KEY_INDEX 
					: sodium_xor_key_pos;
			}

			static std::array<uint8_t, crypto_secretbox_KEYBYTES> key;
			static std::array<uint8_t, crypto_secretbox_NONCEBYTES> nonce;

			std::copy(image_file_vec.begin() + SODIUM_KEY_INDEX, image_file_vec.begin() + SODIUM_KEY_INDEX + crypto_secretbox_KEYBYTES, key.data());
			std::copy(image_file_vec.begin() + NONCE_KEY_INDEX, image_file_vec.begin() + NONCE_KEY_INDEX + crypto_secretbox_NONCEBYTES, nonce.data());

			std::string decrypted_filename;

			const uint16_t ENCRYPTED_FILENAME_INDEX = hasBlueskyOption ? 0x161 : 0x2CF;

			uint16_t filename_xor_key_pos = hasBlueskyOption ? 0x175 : 0x2E3;
	
			uint8_t
				encrypted_filename_length = image_file_vec[ENCRYPTED_FILENAME_INDEX - 1],
				filename_char_pos = 0;

			const std::string ENCRYPTED_FILENAME { image_file_vec.begin() + ENCRYPTED_FILENAME_INDEX, image_file_vec.begin() + ENCRYPTED_FILENAME_INDEX + encrypted_filename_length };

			while (encrypted_filename_length--) {
				decrypted_filename += ENCRYPTED_FILENAME[filename_char_pos++] ^ image_file_vec[filename_xor_key_pos++];
			}
	
			constexpr uint16_t TOTAL_PROFILE_HEADER_SEGMENTS_INDEX 	= 0x2C8;

			const uint16_t 
				ENCRYPTED_FILE_START_INDEX		= hasBlueskyOption ? 0x1D1 : 0x33B,
				FILE_SIZE_INDEX 				= hasBlueskyOption ? 0x1CD : 0x2CA,
				TOTAL_PROFILE_HEADER_SEGMENTS 	= (static_cast<uint16_t>(image_file_vec[TOTAL_PROFILE_HEADER_SEGMENTS_INDEX]) << 8) | static_cast<uint16_t>(image_file_vec[TOTAL_PROFILE_HEADER_SEGMENTS_INDEX + 1]);

			constexpr uint32_t COMMON_DIFF_VAL = 65537; // Size difference between each icc segment profile header.

			uint32_t embedded_file_size = 0;
	
			for (uint8_t i = 0; i < 4; ++i) {
        		embedded_file_size = (embedded_file_size << 8) | static_cast<uint32_t>(image_file_vec[FILE_SIZE_INDEX + i]);
    		}
		
			int32_t last_segment_index = (TOTAL_PROFILE_HEADER_SEGMENTS - 1) * COMMON_DIFF_VAL - 0x16;
	
			// Check embedded data file for corruption, such as missing data segments.
			if (TOTAL_PROFILE_HEADER_SEGMENTS && !hasBlueskyOption) {
				if (last_segment_index > static_cast<int32_t>(image_file_vec.size()) || image_file_vec[last_segment_index] != 0xFF || image_file_vec[last_segment_index + 1] != 0xE2) {
					throw std::runtime_error("File Extraction Error: Missing segments detected. Embedded data file is corrupt!");
				}
			}
	
			std::vector<uint8_t> tmp_vec(image_file_vec.begin() + ENCRYPTED_FILE_START_INDEX, image_file_vec.begin() + ENCRYPTED_FILE_START_INDEX + embedded_file_size);
		
			image_file_vec.swap(tmp_vec);
			std::vector<uint8_t>().swap(tmp_vec);

			std::vector<uint8_t>decrypted_file_vec;

			if (hasBlueskyOption || !TOTAL_PROFILE_HEADER_SEGMENTS) {
				decrypted_file_vec.resize(image_file_vec.size() - crypto_secretbox_MACBYTES);
				if (crypto_secretbox_open_easy(decrypted_file_vec.data(), image_file_vec.data(), image_file_vec.size(), nonce.data(), key.data()) !=0 ) {
					std::cerr << "\nDecryption failed!" << std::endl;
					hasDecryptionFailed = true;
				}	
			} else {		
				const uint32_t ENCRYPTED_FILE_SIZE = static_cast<uint32_t>(image_file_vec.size());
				uint32_t 
					header_index = 0xFCB0, // The first split segment profile header location, this is after the main header/icc profile, which was previously removed.
					index_pos = 0;
	
				std::vector<uint8_t>sanitize_vec; 
				sanitize_vec.reserve(ENCRYPTED_FILE_SIZE);

				constexpr uint8_t PROFILE_HEADER_LENGTH	= 18;

				// We need to avoid including the icc segment profile headers within the decrypted output file.
				// Because we know the total number of profile headers and their location (common difference val), 
				// we can just skip the header bytes when copying the data to the sanitize vector.
        		// This is much faster than having to search for and then using something like vec.erase to remove the header string from the vector.

				while (ENCRYPTED_FILE_SIZE > index_pos) {
					sanitize_vec.emplace_back(image_file_vec[index_pos++]);
					if (index_pos == header_index) {
						index_pos += PROFILE_HEADER_LENGTH; // Skip the header bytes.
						header_index += COMMON_DIFF_VAL;
					}	
				}
				std::vector<uint8_t>().swap(image_file_vec);
			
				decrypted_file_vec.resize(sanitize_vec.size() - crypto_secretbox_MACBYTES);
				if (crypto_secretbox_open_easy(decrypted_file_vec.data(), sanitize_vec.data(), sanitize_vec.size(), nonce.data(), key.data()) !=0 ) {
					std::cerr << "\nDecryption failed!" << std::endl;
					hasDecryptionFailed = true;
				}	
				std::vector<uint8_t>().swap(sanitize_vec);
			}
		
			// ----------------	
		
			std::streampos pin_attempts_index = JDVRIF_SIG_INDEX + INDEX_DIFF - 1;
			 
			if (hasDecryptionFailed) {	
				std::fstream file(args.image_file_path, std::ios::in | std::ios::out | std::ios::binary);
		
				if (pin_attempts_val == 0x90) {
					pin_attempts_val = 0;
				} else {
    				pin_attempts_val++;
				}

			if (pin_attempts_val > 2) {
				file.close();
				std::ofstream file(args.image_file_path, std::ios::out | std::ios::trunc | std::ios::binary);
			} else {
				file.seekp(pin_attempts_index);
				file.write(reinterpret_cast<char*>(&pin_attempts_val), sizeof(pin_attempts_val));
			}
				file.close();
				throw std::runtime_error("File Decryption Error: Invalid recovery PIN or file is corrupt.");
			}
	
			// Inflate data file with Zlib
			zlibFunc(decrypted_file_vec, args.mode, isCompressedFile);
		
			const uint32_t INFLATED_FILE_SIZE = static_cast<uint32_t>(decrypted_file_vec.size());
			// -------------
			if (!INFLATED_FILE_SIZE) {
				throw std::runtime_error("Zlib Compression Error: Output file is empty. Inflating file failed.");
			}

			if (pin_attempts_val != 0x90) {
				std::fstream file(args.image_file_path, std::ios::in | std::ios::out | std::ios::binary);
		
				uint8_t reset_pin_attempts_val = 0x90;

				file.seekp(pin_attempts_index);
				file.write(reinterpret_cast<char*>(&reset_pin_attempts_val), sizeof(reset_pin_attempts_val));

				file.close();
			}

			std::ofstream file_ofs(decrypted_filename, std::ios::binary);

			if (!file_ofs) {
				throw std::runtime_error("Write Error: Unable to write to file. Make sure you have WRITE permissions for this location.");
			}

			file_ofs.write(reinterpret_cast<const char*>(decrypted_file_vec.data()), INFLATED_FILE_SIZE);
			file_ofs.close();
		
			std::vector<uint8_t>().swap(decrypted_file_vec);

			std::cout << "\nExtracted hidden file: " << decrypted_filename << " (" << INFLATED_FILE_SIZE << " bytes).\n\nComplete! Please check your file.\n\n";
			return 0;		
        }
   	}
	catch (const std::runtime_error& e) {
    	std::cerr << "\n" << e.what() << "\n\n";
        return 1;
    }
}
