/* zlib.h -- interface of the 'zlib' general purpose compression library
  version 1.3.1, January 22nd, 2024

  Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
*/

#include "inflateFile.h"
#include <zlib.h>    
#include <algorithm> 
#include <utility>   

const uint32_t inflateFile(std::vector<uint8_t>& vec) {
    constexpr uint32_t BUFSIZE = 2 * 1024 * 1024;

    std::vector<uint8_t> buffer(BUFSIZE); 
    std::vector<uint8_t> inflate_vec;
    inflate_vec.reserve(vec.size() + BUFSIZE);

    z_stream strm = {};
    strm.next_in = vec.data();
    strm.avail_in = static_cast<uint32_t>(vec.size());
    strm.next_out = buffer.data();
    strm.avail_out = BUFSIZE;

    if (inflateInit(&strm) != Z_OK) {
        return 0;
    }

    while (strm.avail_in > 0) {
        int ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) {
            inflateEnd(&strm);
            return 0; 
        }

        if (strm.avail_out == 0) {
            inflate_vec.insert(inflate_vec.end(), buffer.begin(), buffer.end());
            strm.next_out = buffer.data();
            strm.avail_out = BUFSIZE;
        }
    }

    int ret;
    do {
        ret = inflate(&strm, Z_FINISH);
        size_t bytes_written = BUFSIZE - strm.avail_out;
        inflate_vec.insert(inflate_vec.end(), buffer.begin(), buffer.begin() + bytes_written);
        strm.next_out = buffer.data();
        strm.avail_out = BUFSIZE;
    } while (ret == Z_OK);

    inflateEnd(&strm);

    vec = std::move(inflate_vec);
    return static_cast<uint32_t>(vec.size());
}
