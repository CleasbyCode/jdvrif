// zlib function, see https://zlib.net/
void deflateFile(std::vector<uint_fast8_t>& Vec) {
	
	constexpr uint_fast32_t
		MASSIVE_FILE_SIZE = 734003200, // > 700MB
		LARGE_FILE_SIZE	  = 314572800, // > 300MB
		MEDIUM_FILE_SIZE  = 104857600, // > 100MB
		BUFSIZE = 2097152;
	
	const uint_fast32_t VEC_SIZE = static_cast<uint_fast32_t>(Vec.size());

	uint_fast8_t* buffer{ new uint_fast8_t[BUFSIZE] };
	
	std::vector<uint_fast8_t>Deflate_Vec;

	z_stream strm;
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.next_in = Vec.data();
	strm.avail_in = VEC_SIZE;
	strm.next_out = buffer;
	strm.avail_out = BUFSIZE;

	int_fast8_t compression_level;

	if (VEC_SIZE > MASSIVE_FILE_SIZE) {
	    compression_level = Z_NO_COMPRESSION;
	} else if (VEC_SIZE > LARGE_FILE_SIZE) {
	    compression_level = Z_BEST_SPEED;
	} else if (VEC_SIZE > MEDIUM_FILE_SIZE) {
	    compression_level = Z_DEFAULT_COMPRESSION;
	} else {
	    compression_level = Z_BEST_COMPRESSION;
	}

	deflateInit(&strm, compression_level);

	while (strm.avail_in) {
		deflate(&strm, Z_NO_FLUSH);
		
		if (!strm.avail_out) {
			Deflate_Vec.insert(Deflate_Vec.end(), buffer, buffer + BUFSIZE);
			strm.next_out = buffer;
			strm.avail_out = BUFSIZE;
		} else {
			break;
		}
	}
	
	deflate(&strm, Z_FINISH);
	Deflate_Vec.insert(Deflate_Vec.end(), buffer, buffer + BUFSIZE - strm.avail_out);
	deflateEnd(&strm);

	delete[] buffer;

	Vec.swap(Deflate_Vec);		
}
