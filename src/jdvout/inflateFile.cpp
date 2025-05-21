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
#include <iterator>  
#include <utility>   
#include <new>

const uint32_t inflateFile(std::vector<uint8_t>& vec) {
	constexpr uint32_t BUFSIZE = 2 * 1024 * 1024;

	uint8_t* buffer{ new uint8_t[BUFSIZE] };
	
	std::vector<uint8_t>inflate_vec;
	inflate_vec.reserve(vec.size() + BUFSIZE);

	z_stream strm;
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.next_in = vec.data();
	strm.avail_in = static_cast<uint32_t>(vec.size());
	strm.next_out = buffer;
	strm.avail_out = BUFSIZE;

	inflateInit(&strm);

	while (strm.avail_in) {
		inflate(&strm, Z_NO_FLUSH);
		if (!strm.avail_out) {
			std::copy_n(buffer, BUFSIZE, std::back_inserter(inflate_vec));
			strm.next_out = buffer;
			strm.avail_out = BUFSIZE;
		} else {
		    break;
		}
	}

	inflate(&strm, Z_FINISH);
	std::copy_n(buffer, BUFSIZE - strm.avail_out, std::back_inserter(inflate_vec));
	inflateEnd(&strm);
	
	delete[] buffer;
	vec = std::move(inflate_vec);	
	std::vector<uint8_t>().swap(inflate_vec);

	return(static_cast<uint32_t>(vec.size()));
}
