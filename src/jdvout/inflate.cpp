// zlib function, see https://zlib.net/

uint_fast32_t inflateFile(std::vector<uint_fast8_t>& Vec) {

	std::vector<uint_fast8_t>Buffer_Vec;

	constexpr uint_fast32_t BUFSIZE = 524288;

	uint_fast8_t* temp_buffer{ new uint_fast8_t[BUFSIZE] };

	z_stream strm;
	strm.zalloc = 0;
	strm.zfree = 0;
	strm.next_in = Vec.data();
	strm.avail_in = static_cast<uint_fast32_t>(Vec.size());
	strm.next_out = temp_buffer;
	strm.avail_out = BUFSIZE;

	inflateInit(&strm);

	while (strm.avail_in) {
		inflate(&strm, Z_NO_FLUSH);

		if (!strm.avail_out) {
			Buffer_Vec.insert(Buffer_Vec.end(), temp_buffer, temp_buffer + BUFSIZE);
			strm.next_out = temp_buffer;
			strm.avail_out = BUFSIZE;
		} else {
		    break;
		}
	}

	inflate(&strm, Z_FINISH);
	Buffer_Vec.insert(Buffer_Vec.end(), temp_buffer, temp_buffer + BUFSIZE - strm.avail_out);
	inflateEnd(&strm);

	Vec.swap(Buffer_Vec);
	delete[] temp_buffer;

	return static_cast<uint_fast32_t>(Vec.size());
}
