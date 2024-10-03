// zlib function, see https://zlib.net/
uint32_t deflateFile(std::vector<uint8_t>& Vec, bool isCompressedFile) {
			
	constexpr uint32_t
		BUFSIZE = 2097152,	
		LARGE_FILE_SIZE	  = 524288000,  //  > 500MB.
		MEDIUM_FILE_SIZE  = 157286400;  //  > 150MB. 

	const uint32_t VEC_SIZE = static_cast<uint32_t>(Vec.size());

	uint8_t* buffer{ new uint8_t[BUFSIZE] };
	
	std::vector<uint8_t>Deflate_Vec;
	Deflate_Vec.reserve(VEC_SIZE + BUFSIZE);

	z_stream strm;
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.next_in = Vec.data();
	strm.avail_in = VEC_SIZE;
	strm.next_out = buffer;
	strm.avail_out = BUFSIZE;

	int8_t compression_level;

	if (isCompressedFile) {
	    compression_level = Z_NO_COMPRESSION;
	} else if (VEC_SIZE > LARGE_FILE_SIZE) {
	    compression_level = Z_BEST_SPEED;
	} else if (VEC_SIZE > MEDIUM_FILE_SIZE) {
	    compression_level = Z_DEFAULT_COMPRESSION;
	} else {
	    compression_level = Z_BEST_COMPRESSION;
	}

	deflateInit(&strm, compression_level);

	while (strm.avail_in)
	{
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

	return (static_cast<uint32_t>(Vec.size()));
}