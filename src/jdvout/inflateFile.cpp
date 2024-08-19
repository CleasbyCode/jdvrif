// zlib function, see https://zlib.net/
uint32_t inflateFile(std::vector<uint8_t>& Vec) {

	std::vector<uint8_t>Inflate_Vec;
	
	constexpr uint32_t BUFSIZE = 2097152;

	uint8_t* buffer{ new uint8_t[BUFSIZE] };

	z_stream strm;
	strm.zalloc = 0;
	strm.zfree = 0;
	strm.next_in = Vec.data();
	strm.avail_in = static_cast<uint32_t>(Vec.size());
	strm.next_out = buffer;
	strm.avail_out = BUFSIZE;

	inflateInit(&strm);

	while (strm.avail_in) {
		inflate(&strm, Z_NO_FLUSH);

		if (!strm.avail_out) {
			Inflate_Vec.insert(Inflate_Vec.end(), buffer, buffer + BUFSIZE);
			strm.next_out = buffer;
			strm.avail_out = BUFSIZE;
		} else {
		    break;
		}
	}

	inflate(&strm, Z_FINISH);
	Inflate_Vec.insert(Inflate_Vec.end(), buffer, buffer + BUFSIZE - strm.avail_out);
	inflateEnd(&strm);

	Vec.swap(Inflate_Vec);
	delete[] buffer;

	return static_cast<uint32_t>(Vec.size());
}
