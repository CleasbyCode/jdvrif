// zlib function, see https://zlib.net/
void deflateFile(std::vector<uint_fast8_t>& Vec) {
	
	constexpr uint_fast32_t BUFSIZE = 2097152;
	uint_fast8_t* buffer{ new uint_fast8_t[BUFSIZE] };
	
	std::vector<uint_fast8_t>Deflate_Vec;

	z_stream strm;
	strm.zalloc = 0;
	strm.zfree = 0;
	strm.next_in = Vec.data();
	strm.avail_in = static_cast<uint_fast32_t>(Vec.size());
	strm.next_out = buffer;
	strm.avail_out = BUFSIZE;
	
	deflateInit(&strm, 6); // Standard compression level 6
	
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
}
