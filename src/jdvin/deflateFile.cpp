// zlib function, see https://zlib.net/
void deflateFile(std::vector<uint8_t>& Vec, const std::string DATA_FILE_EXTENSION) {
	
	std::vector<uint8_t>Deflate_Vec;

	constexpr uint32_t BUFSIZE = 2097152;

	uint8_t* buffer{ new uint8_t[BUFSIZE] };

	z_stream strm;
	strm.zalloc = 0;
	strm.zfree = 0;
	strm.next_in = Vec.data();
	strm.avail_in = static_cast<uint32_t>(Vec.size());
	strm.next_out = buffer;
	strm.avail_out = BUFSIZE;

	if (DATA_FILE_EXTENSION == ".zip" || DATA_FILE_EXTENSION == ".rar") {
		deflateInit(&strm, 0); // Compression off.
	} else {
		deflateInit(&strm, 6); // Compression level 6
	}
	
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
	
	Vec.swap(Deflate_Vec);
	delete[] buffer;
}
